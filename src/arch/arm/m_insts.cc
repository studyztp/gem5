/*
 * Copyright (c) 2026 University of California, Davis and Cornell University
 * All rights reserved
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met: redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer;
 * redistributions in binary form must reproduce the above copyright
 * notice, this list of conditions and the following disclaimer in the
 * documentation and/or other materials provided with the distribution;
 * neither the name of the copyright holders nor the names of its
 * contributors may be used to endorse or promote products derived from
 * this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

/**
 * @file
 * M-profile instruction implementations.
 *
 * Each execute() method accesses only M-profile registers (MISCREG_M_*)
 * via ThreadContext.  No CPSR/SPSR access anywhere in this file.
 */

#include "arch/arm/m_insts.hh"

#include <sstream>

#include "arch/arm/m_faults.hh"
#include "arch/arm/m_interrupts.hh"
#include "arch/arm/m_system.hh"
#include "arch/arm/pcstate.hh"
#include "arch/arm/regs/int.hh"
#include "arch/arm/regs/misc.hh"
#include "arch/arm/semihosting.hh"
#include "arch/generic/memhelpers.hh"
#include "base/trace.hh"
#include "cpu/base.hh"
#include "cpu/exec_context.hh"
#include "cpu/thread_context.hh"
#include "debug/MProfileStacking.hh"
#include "dev/arm/m_profile_scs.hh"
#include "mem/request.hh"

namespace gem5
{

namespace ArmISA
{

// =========================================================================
// SYSm → MiscRegIndex mapping for M-profile MRS/MSR
// =========================================================================
//
// The SYSm field (8 bits from the instruction encoding) selects which
// M-profile special register to access.  Defined in DDI0403E
// Table B5-1 (for reads) and Table B5-2 (for writes).
//
// SYSm 0-7 all map to MISCREG_M_XPSR — the masking of which fields
// are visible is handled separately in maskXpsrForSysM().

static MiscRegIndex
sysMToMiscReg(uint8_t sysM)
{
    switch (sysM) {
      case 0 ... 7:  return MISCREG_M_XPSR;  // PSR variants
      case 8:        return MISCREG_M_MSP;
      case 9:        return MISCREG_M_PSP;
      case 16:       return MISCREG_M_PRIMASK;
      case 17:       return MISCREG_M_BASEPRI;      // ARMv7-M only
      case 18:       return MISCREG_M_BASEPRI_MAX;  // ARMv7-M only
      case 19:       return MISCREG_M_FAULTMASK;    // ARMv7-M only
      case 20:       return MISCREG_M_CONTROL;
      default:       return NUM_MISCREGS;  // undefined → fault
    }
}

// Mask xPSR value based on SYSm to return only the requested fields.
// DDI0403E B5.2.1: different SYSm values expose different subsets.
static RegVal
maskXpsrForSysM(uint8_t sysM, RegVal xpsr)
{
    // APSR bits: N[31], Z[30], C[29], V[28], Q[27], GE[19:16]
    constexpr uint32_t APSR_MASK = 0xF80F0000;
    // IPSR bits: exception number [8:0]
    constexpr uint32_t IPSR_MASK = 0x000001FF;
    // EPSR bits: ICI/IT[26:25,15:10], T[24]
    constexpr uint32_t EPSR_MASK = 0x0700FC00;

    switch (sysM) {
      case 0:  return xpsr & APSR_MASK;               // APSR
      case 1:  return xpsr & (IPSR_MASK | APSR_MASK); // IAPSR
      case 2:  return xpsr & (EPSR_MASK | APSR_MASK); // EAPSR
      case 3:  return xpsr;                            // XPSR (all)
      case 5:  return xpsr & IPSR_MASK;               // IPSR
      case 6:  return xpsr & EPSR_MASK;               // EPSR
      case 7:  return xpsr & (IPSR_MASK | EPSR_MASK); // IEPSR
      default: return xpsr;
    }
}

// SYSm name for disassembly
static const char *
sysMName(uint8_t sysM)
{
    static const char *names[] = {
        "APSR", "IAPSR", "EAPSR", "XPSR",
        "?4", "IPSR", "EPSR", "IEPSR",
        "MSP", "PSP", "?10", "?11",
        "?12", "?13", "?14", "?15",
        "PRIMASK", "BASEPRI", "BASEPRI_MAX", "FAULTMASK",
        "CONTROL"
    };
    if (sysM <= 20)
        return names[sysM];
    return "UNKNOWN";
}

// =========================================================================
// MrsMProfile::execute — MRS Rd, <spec_reg>
// =========================================================================

Fault
MrsMProfile::execute(ExecContext *xc,
                     trace::InstRecord *traceData) const
{
    ThreadContext *tc = xc->tcBase();
    MiscRegIndex reg = sysMToMiscReg(sysM);

    if (reg == NUM_MISCREGS) {
        return std::make_shared<ArmMFault>(MPEXC_USAGEFAULT);
    }

    RegVal val;
    if (sysM <= 7) {
        // xPSR variants: read full xPSR, then mask to requested fields.
        // BUG-5 fix: sync CC flat regs → xPSR before reading so that
        // MRS APSR returns live NZCV/GE flags, not stale values from
        // the last MSR write.  Without this, __get_IPSR() and any
        // firmware reading APSR sees incorrect condition flags.
        syncCCRegsToXpsr(tc);
        // Uses readMiscReg (not NoEffect) to sync T bit from PCState.
        val = tc->readMiscReg(MISCREG_M_XPSR);
        val = maskXpsrForSysM(sysM, val);
    } else if (sysM == 8 || sysM == 9) {
        // MSP (SYSm=8) or PSP (SYSm=9): read from architectural R13.
        //
        // On real Cortex-M, R13 IS MSP or PSP depending on
        // CONTROL.SPSEL.  In gem5, R13 (int reg) is the authoritative
        // SP; MISCREG_M_MSP/PSP are sync copies.
        //
        // MRS MSP: if CONTROL.SPSEL=0 (MSP active), return R13.
        //          if CONTROL.SPSEL=1 (PSP active), return saved MSP.
        // MRS PSP: if CONTROL.SPSEL=1 (PSP active), return R13.
        //          if CONTROL.SPSEL=0 (MSP active), return saved PSP.
        RegVal control = tc->readMiscRegNoEffect(MISCREG_M_CONTROL);
        bool spsel = bits(control, 1);  // CONTROL.SPSEL
        if ((sysM == 8 && !spsel) || (sysM == 9 && spsel)) {
            // Reading the currently active SP → read from R13
            val = tc->getReg(int_reg::Sp);
        } else {
            // Reading the inactive SP → read from misc reg
            val = tc->readMiscRegNoEffect(reg);
        }
    } else {
        val = tc->readMiscReg(reg);
    }

    tc->setReg(RegId(intRegClass, dest), val);

    if (traceData)
        traceData->setData(val);

    return NoFault;
}

std::string
MrsMProfile::generateDisassembly(Addr pc,
    const loader::SymbolTable *symtab) const
{
    std::stringstream ss;
    ss << "  mrs   ";
    printIntReg(ss, dest);
    ss << ", " << sysMName(sysM);
    return ss.str();
}

// =========================================================================
// MsrMProfile::execute — MSR <spec_reg>, Rn
// =========================================================================

Fault
MsrMProfile::execute(ExecContext *xc,
                     trace::InstRecord *traceData) const
{
    ThreadContext *tc = xc->tcBase();
    MiscRegIndex reg = sysMToMiscReg(sysM);

    if (reg == NUM_MISCREGS) {
        return std::make_shared<ArmMFault>(MPEXC_USAGEFAULT);
    }

    // Read source register directly (see MrsMProfile comment for why
    // we use tc->getReg instead of xc->getRegOperand).
    RegVal val = tc->getReg(RegId(intRegClass, op1));

    if (sysM <= 7) {
        // xPSR write: only APSR bits (N,Z,C,V,Q,GE) are writable
        // via MSR.  IPSR and EPSR bits are read-only.
        // DDI0403E B5.2.1: "The MSR instruction can write the
        // N, Z, C, V, Q, and GE[3:0] bits."
        constexpr uint32_t APSR_WRITE_MASK = 0xF80F0000;
        RegVal xpsr = tc->readMiscRegNoEffect(MISCREG_M_XPSR);
        xpsr = (xpsr & ~APSR_WRITE_MASK) | (val & APSR_WRITE_MASK);
        tc->setMiscRegNoEffect(MISCREG_M_XPSR, xpsr);
        // BUG-5 fix: sync the new NZCV/GE from xPSR → CC flat regs.
        // Without this, subsequent conditional instructions (BEQ, BNE)
        // read stale flags from CC regs and ignore the MSR write.
        syncXpsrToCCRegs(tc);
    } else if (sysM == 8 || sysM == 9) {
        // MSR MSP/PSP: sync with architectural R13.
        //
        // R13 is the authoritative SP.  On MSR write:
        //   MSR MSP: if SPSEL=0 (MSP active), also update R13.
        //            Always update MISCREG_M_MSP.
        //   MSR PSP: if SPSEL=1 (PSP active), also update R13.
        //            Always update MISCREG_M_PSP.
        //
        // This keeps R13 in sync with the active SP pointer.
        RegVal control = tc->readMiscRegNoEffect(MISCREG_M_CONTROL);
        bool spsel = bits(control, 1);
        tc->setMiscRegNoEffect(reg, val);  // always update misc reg
        if ((sysM == 8 && !spsel) || (sysM == 9 && spsel)) {
            // Writing to the currently active SP → also update R13
            tc->setReg(int_reg::Sp, val);
        }
    } else {
        // PRIMASK, BASEPRI, BASEPRI_MAX, FAULTMASK, CONTROL
        // Routes through MISA::setMiscReg which handles BASEPRI_MAX
        // conditional-write and other special semantics.
        tc->setMiscReg(reg, val);

        // Notify SCS of mask register changes so it can update its
        // cached mask state (primask, faultmask, basepri) without
        // needing to read misc regs on every priority check.
        if (reg == MISCREG_M_PRIMASK || reg == MISCREG_M_BASEPRI ||
            reg == MISCREG_M_BASEPRI_MAX || reg == MISCREG_M_FAULTMASK) {
            auto *msys = dynamic_cast<ArmMSystem *>(
                tc->getSystemPtr());
            if (msys && msys->getSCS())
                msys->getSCS()->setupMask(reg, (int16_t)val);
        }
    }

    if (traceData)
        traceData->setData(val);

    return NoFault;
}

std::string
MsrMProfile::generateDisassembly(Addr pc,
    const loader::SymbolTable *symtab) const
{
    std::stringstream ss;
    ss << "  msr   " << sysMName(sysM) << ", ";
    printIntReg(ss, op1);
    return ss.str();
}

// =========================================================================
// CpsMProfile::execute — CPSID/CPSIE i/f
// =========================================================================

Fault
CpsMProfile::execute(ExecContext *xc,
                     trace::InstRecord *traceData) const
{
    ThreadContext *tc = xc->tcBase();

    auto *msys = dynamic_cast<ArmMSystem *>(tc->getSystemPtr());
    MProfileSCS *scs = (msys) ? msys->getSCS() : nullptr;

    if (affectI) {
        // PRIMASK: 1 = disable all configurable-priority exceptions
        int16_t val = disable ? 1 : 0;
        tc->setMiscReg(MISCREG_M_PRIMASK, val);
        if (scs) scs->setupMask(MISCREG_M_PRIMASK, val);
    }

    if (affectF) {
        // FAULTMASK: 1 = disable all exceptions except NMI
        // Only available on ARMv7-M (M3/M4/M7), not M0.
        int16_t val = disable ? 1 : 0;
        tc->setMiscReg(MISCREG_M_FAULTMASK, val);
        if (scs) scs->setupMask(MISCREG_M_FAULTMASK, val);
    }

    return NoFault;
}

std::string
CpsMProfile::generateDisassembly(Addr pc,
    const loader::SymbolTable *symtab) const
{
    std::stringstream ss;
    ss << (disable ? "  cpsid " : "  cpsie ");
    if (affectI) ss << "i";
    if (affectF) ss << "f";
    return ss.str();
}

// =========================================================================
// BxMProfile::execute — BX Rm with EXC_RETURN detection
// =========================================================================

Fault
BxMProfile::execute(ExecContext *xc,
                    trace::InstRecord *traceData) const
{
    ThreadContext *tc = xc->tcBase();
    RegVal target = tc->getReg(RegId(intRegClass, op1));

    // EXC_RETURN detection (DDI0403E B1.5.8):
    // If bits[31:4] are all 1s, this is an exception return magic value,
    // not a branch address.  Valid range: 0xFFFFFFF0–0xFFFFFFFF.
    if ((target & 0xFFFFFFF0) == 0xFFFFFFF0) {
        // Exception return: deactivate via interrupt controller, then
        // unstack CPU state.  MProfileInterrupts::excReturn() handles both.
        DPRINTF(MProfileStacking,
                "BxMProfile: EXC_RETURN detected target=%#x, "
                "triggering exception return\n", target);
        auto *mintr = dynamic_cast<MProfileInterrupts *>(
            tc->getCpuPtr()->getInterruptController(tc->threadId()));
        assert(mintr && "M-profile CPU must use MProfileInterrupts");
        mintr->excReturn(tc, (uint32_t)target);
        return NoFault;
    }

    // M-profile is always Thumb.  BX with bit[0]=0 is a UsageFault
    // (INVSTATE).  DDI0403E A7.7.20: "If the value of bit[0] of Rm
    // is 0, the result is UNPREDICTABLE."  On M-profile, the ARMv7-M
    // ARM clarifies this is INVSTATE UsageFault (B1.5.8).
    if (!(target & 1)) {
        return std::make_shared<ArmMFault>(MPEXC_USAGEFAULT);
    }

    // Sanity check: M-profile must always be in Thumb mode, never AArch64.
    auto pc = tc->pcState().as<PCState>();
    assert(pc.thumb() && "M-profile PC must be in Thumb mode");
    assert(!pc.aarch64() && "M-profile cannot be in AArch64 mode");

    // Normal branch — set NPC to target.
    // In gem5's execution model, branches must write to pc.npc(), NOT
    // pc.set().  pc.set(val) sets _pc=val AND _npc=val+instSize; after
    // advancePC() calls advance(), _pc becomes _npc = val+2 (off by two).
    // pc.npc(val) sets only _npc=val; advance() then gives _pc=val. Correct.
    // DDI0403E §A7.7.20: PC = R[m]<31:1>:0
    pc.nextThumb(target & 1);   // thumb state for the branch target
    pc.npc(target & ~(Addr)1);  // NPC = branch target (advance() copies to PC)
    tc->pcState(pc);

    return NoFault;
}

std::string
BxMProfile::generateDisassembly(Addr pc,
    const loader::SymbolTable *symtab) const
{
    std::stringstream ss;
    ss << "  bx    ";
    printIntReg(ss, op1);
    return ss.str();
}

// =========================================================================
// BlxRegMProfile::execute — BLX Rm with EXC_RETURN detection
// =========================================================================

Fault
BlxRegMProfile::execute(ExecContext *xc,
                        trace::InstRecord *traceData) const
{
    ThreadContext *tc = xc->tcBase();
    RegVal target = tc->getReg(RegId(intRegClass, op1));

    // DDI0403E B1.5.8: BLX does NOT trigger EXC_RETURN.
    // Only BX, POP {PC}, LDM {PC}, and LDR Rd=PC can trigger exception
    // return.  BLX with an EXC_RETURN value is UNPREDICTABLE; we treat
    // it as a normal branch (which will fault if the address is unmapped).

    // Set LR to return address (next instruction after BLX).
    // Thumb-16 BLX is 2 bytes.  Bit[0]=1 indicates Thumb state.
    auto pc = tc->pcState().as<PCState>();
    RegVal lr = pc.instAddr() + 2;
    lr |= 1;
    tc->setReg(int_reg::Lr, lr);

    // Branch to target — same pc.npc() rationale as BxMProfile above.
    // DDI0403E §A7.7.19: PC = R[m]<31:1>:0; LR = (return_addr)|1
    pc.thumb(target & 1);
    pc.nextThumb(target & 1);
    pc.npc(target & ~(Addr)1);  // NPC = branch target (not pc.set!)
    tc->pcState(pc);

    return NoFault;
}

std::string
BlxRegMProfile::generateDisassembly(Addr pc,
    const loader::SymbolTable *symtab) const
{
    std::stringstream ss;
    ss << "  blx   ";
    printIntReg(ss, op1);
    return ss.str();
}

// =========================================================================
// SvcMProfile::execute — SVC #imm8
// =========================================================================

Fault
SvcMProfile::execute(ExecContext *xc,
                     trace::InstRecord *traceData) const
{
    // M-profile SVC generates SVCall exception (exception number 11).
    // The exception entry mechanism (ArmMFault::invoke in m_faults.cc)
    // pushes the hardware exception frame and branches to the handler.
    return std::make_shared<ArmMFault>(MPEXC_SVCALL);
}

std::string
SvcMProfile::generateDisassembly(Addr pc,
    const loader::SymbolTable *symtab) const
{
    std::stringstream ss;
    ss << "  svc   #" << (unsigned)svcImm;
    return ss.str();
}

// =========================================================================
// WfiMProfile::execute — WFI (Wait for Interrupt)
// =========================================================================

Fault
WfiMProfile::execute(ExecContext *xc,
                     trace::InstRecord *traceData) const
{
    // M-profile WFI: sleep until an interrupt is pending.
    // Unlike A-profile, no hypervisor trap checks (no HCR/SCR).
    ThreadContext *tc = xc->tcBase();
    tc->quiesce();
    return NoFault;
}

std::string
WfiMProfile::generateDisassembly(Addr pc,
    const loader::SymbolTable *symtab) const
{
    return "  wfi";
}

// =========================================================================
// WfeMProfile::execute — WFE (Wait for Event)
// =========================================================================

Fault
WfeMProfile::execute(ExecContext *xc,
                     trace::InstRecord *traceData) const
{
    // M-profile WFE: check event register.
    // For MVP, treat as hint-NOP (sleep for 1 cycle).
    // Full event register tracking deferred.
    ThreadContext *tc = xc->tcBase();
    Tick next_cycle = tc->getCpuPtr()->nextCycle();
    tc->quiesceTick(next_cycle + 1);
    return NoFault;
}

std::string
WfeMProfile::generateDisassembly(Addr pc,
    const loader::SymbolTable *symtab) const
{
    return "  wfe";
}

// =========================================================================
// MProfileUndefined::execute — blocked A-profile instruction
// =========================================================================

Fault
MProfileUndefined::execute(ExecContext *xc,
                           trace::InstRecord *traceData) const
{
    // A-profile-only instruction on M-profile → UsageFault.
    // DDI0403E B1.5.3: undefined instruction generates UsageFault.
    // CFSR.UNDEFINSTR bit is set by the exception entry logic.
    warn("M-profile: undefined instruction (A-profile only): %s "
         "at PC=%#x, encoding=%#x\n",
         reason, xc->pcState().instAddr(), (uint32_t)machInst);
    return std::make_shared<ArmMFault>(MPEXC_USAGEFAULT);
}

std::string
MProfileUndefined::generateDisassembly(Addr pc,
    const loader::SymbolTable *symtab) const
{
    std::stringstream ss;
    ss << "  undefined (" << reason << ")";
    return ss.str();
}

// =========================================================================
// ExcReturnFromPC::execute — POP {PC} / LDM {PC} exception return
// =========================================================================

Fault
ExcReturnFromPC::execute(ExecContext *xc,
                         trace::InstRecord *traceData) const
{
    ThreadContext *tc = xc->tcBase();
    // The EXC_RETURN value was the address the CPU tried to fetch from.
    DPRINTF(MProfileStacking,
            "ExcReturnFromPC: excReturnVal=%#x, "
            "triggering exception return\n", excReturnVal);
    // Exception return: deactivate via interrupt controller, then
    // unstack CPU state.  MProfileInterrupts::excReturn() handles both.
    auto *mintr = dynamic_cast<MProfileInterrupts *>(
        tc->getCpuPtr()->getInterruptController(tc->threadId()));
    assert(mintr && "M-profile CPU must use MProfileInterrupts");
    mintr->excReturn(tc, excReturnVal);
    return NoFault;
}

std::string
ExcReturnFromPC::generateDisassembly(Addr pc,
    const loader::SymbolTable *symtab) const
{
    std::stringstream ss;
    ss << "  exc_return #" << std::hex << excReturnVal;
    return ss.str();
}

// =========================================================================
// BkptSemiMProfile::execute — BKPT #0xAB semihosting
// =========================================================================

Fault
BkptSemiMProfile::execute(ExecContext *xc,
                          trace::InstRecord *traceData) const
{
    ThreadContext *tc = xc->tcBase();

    // Get semihosting handler from ArmMSystem.
    auto *sys = dynamic_cast<ArmMSystem *>(tc->getSystemPtr());
    if (!sys || !sys->haveSemihosting()) {
        // No semihosting configured — treat as normal BKPT (debug fault).
        // Let the standard decoder handle it on fallthrough.
        return std::make_shared<UndefinedInstruction>(
            machInst, false, ExceptionClass::SOFTWARE_BREAKPOINT);
    }

    // Dispatch the semihosting call.
    // ABI: R0 = operation code, R1 = parameter block pointer.
    // call32() reads R0/R1 internally via ArmSemihosting::Abi32.
    sys->semihosting->call32(tc, true);

    return NoFault;
}

std::string
BkptSemiMProfile::generateDisassembly(Addr pc,
    const loader::SymbolTable *symtab) const
{
    return "  bkpt  #0xab  ; semihosting";
}

// =========================================================================
// LdrexMProfile::execute — LDREX / LDREXB / LDREXH
// =========================================================================
//
// M-profile replacement for the ISA-generated LDREX instruction classes.
// The generated versions call ArmISA::ISA::getSelfDebug() which does a
// static_cast<ISA*> on MISA*, causing undefined behavior.  This version
// performs the same LLSC memory read but skips the SelfDebug call.

Fault
LdrexMProfile::execute(ExecContext *xc,
                       trace::InstRecord *traceData) const
{
    ThreadContext *tc = xc->tcBase();
    Addr addr = tc->getReg(RegId(intRegClass, base)) + imm;

    // Perform the exclusive load via the standard memory interface.
    // Request::LLSC triggers handleLockedRead() on the ISA (MISA),
    // which records the exclusive address in the local monitor.
    // The low bits of memAccessFlags encode alignment requirements
    // (matching the ISA-generated code: 2=word, 1=half, 0=byte).
    Request::Flags memFlags = Request::LLSC;
    if (accessSize == 4)
        memFlags = Request::Flags(2 | Request::LLSC);
    else if (accessSize == 2)
        memFlags = Request::Flags(1 | Request::LLSC);

    RegVal result = 0;
    if (accessSize == 4) {
        uint32_t data = 0;
        Fault fault = readMemAtomicLE(xc, traceData, addr, data, memFlags);
        if (fault != NoFault)
            return fault;
        result = data;
    } else if (accessSize == 2) {
        uint16_t data = 0;
        Fault fault = readMemAtomicLE(xc, traceData, addr, data, memFlags);
        if (fault != NoFault)
            return fault;
        result = data;
    } else {
        uint8_t data = 0;
        Fault fault = readMemAtomicLE(xc, traceData, addr, data, memFlags);
        if (fault != NoFault)
            return fault;
        result = data;
    }

    tc->setReg(RegId(intRegClass, dest), result);

    if (traceData)
        traceData->setData(result);

    return NoFault;
}

std::string
LdrexMProfile::generateDisassembly(Addr pc,
    const loader::SymbolTable *symtab) const
{
    std::stringstream ss;
    if (accessSize == 1)
        ss << "  ldrexb  ";
    else if (accessSize == 2)
        ss << "  ldrexh  ";
    else
        ss << "  ldrex   ";
    printIntReg(ss, dest);
    ss << ", [";
    printIntReg(ss, base);
    if (imm)
        ss << ", #" << imm;
    ss << "]";
    return ss.str();
}

} // namespace ArmISA
} // namespace gem5
