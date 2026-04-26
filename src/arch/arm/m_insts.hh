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

#ifndef __ARCH_ARM_M_INSTS_HH__
#define __ARCH_ARM_M_INSTS_HH__

/** @file
 * M-profile instruction classes.
 *
 * These classes are returned by MDecoder::tryMProfileDecode() for
 * instructions that have different behavior on M-profile versus
 * A-profile.  They access only M-profile registers (MISCREG_M_*)
 * and never touch CPSR/SPSR.
 *
 * Each class documents the ARMv7-M reference for that instruction.
 */

#include <type_traits>

#include "arch/arm/insts/macromem.hh"
#include "arch/arm/insts/pred_inst.hh"
#include "arch/arm/regs/cc.hh"      // cc_reg::Nz / C / V (flat NZCV)
#include "arch/arm/regs/int.hh"
#include "arch/arm/regs/misc.hh"
#include "arch/arm/regs/misc_types.hh"
#include "arch/arm/utility.hh"
#include "cpu/thread_context.hh"

namespace gem5
{

namespace ArmISA
{

/**
 * IT-block predicate gate for hand-written M-profile instructions.
 *
 * Returns true if the instruction should commit. Returns false if
 * the architectural condition is FALSE — per ARM ARM A6.1.4 the
 * instruction has no effect in that case.
 *
 * Every M-profile instruction class in this directory overrides
 * execute() (and sometimes initiateAcc()) to bypass the auto-
 * generated A-profile decode path. None of them honor IT-block
 * predication unless they call this helper at the top of every
 * such method:
 *
 *   if (!mProfilePredicateHolds(xc->tcBase(), condCode))
 *       return NoFault;
 *
 * `condCode` is populated by PredOp's constructor (insts/
 * pred_inst.hh:226-232) from machInst.itstateCond when in an IT
 * block, else from machInst.condCode (which is COND_AL/COND_UC
 * for unpredicated Thumb-2 ops). For non-predicated instructions
 * this returns true, preserving today's behavior — only
 * predicated-but-condition-false instructions are now suppressed.
 *
 * NZCV source: in this fork the LIVE NZCV lives in the per-thread
 * CC flat regs (cc_reg::Nz / C / V). xPSR's NZCV/GE bits are a
 * sync copy that goes stale between syncCCRegsToXpsr() calls —
 * see m_faults.cc syncCCRegsToXpsr / syncXpsrToCCRegs and the
 * BUG-5 commit. Reading cc_reg directly avoids the staleness.
 *
 * cc_reg::Nz packs (N << 1) | Z in 2 bits — exactly what
 * testPredicate's nz argument expects.
 */
inline bool
mProfilePredicateHolds(ThreadContext *tc, ConditionCode condCode)
{
    return testPredicate(tc->getReg(cc_reg::Nz),
                         tc->getReg(cc_reg::C),
                         tc->getReg(cc_reg::V),
                         condCode);
}

/**
 * M-profile MRS: Move from Special Register.
 *
 * MRS Rd, <spec_reg>
 * Thumb-32 encoding T1: 11110011111 (0)SYSm[7:0] Rd[11:8] 10(0)00000
 *
 * The SYSm field (bits[7:0] of the instruction, mapped to
 * bits[11:8] and [4] by the encoding) selects which M-profile
 * special register to read:
 *
 *   SYSm      Register       Reference
 *   ────      ────────       ─────────
 *   0b00000   APSR           DDI0403E B5.2.1
 *   0b00001   IAPSR          DDI0403E B5.2.1
 *   0b00010   EAPSR          DDI0403E B5.2.1
 *   0b00011   XPSR           DDI0403E B5.2.1
 *   0b00101   IPSR           DDI0403E B5.2.1
 *   0b00110   EPSR           DDI0403E B5.2.1
 *   0b00111   IEPSR          DDI0403E B5.2.1
 *   0b01000   MSP            DDI0403E B5.2.2
 *   0b01001   PSP            DDI0403E B5.2.2
 *   0b10000   PRIMASK        DDI0403E B5.2.3
 *   0b10001   BASEPRI        DDI0403E B5.2.4 (ARMv7-M only)
 *   0b10010   BASEPRI_MAX    DDI0403E B5.2.5 (ARMv7-M only)
 *   0b10011   FAULTMASK      DDI0403E B5.2.6 (ARMv7-M only)
 *   0b10100   CONTROL        DDI0403E B5.2.7
 *
 * Reference: DDI0403E A7.7.41 (MRS)
 */
class MrsMProfile : public PredOp
{
  private:
    RegId srcRegIdxArr[1];  // placeholder (misc reg not tracked here)
    RegId destRegIdxArr[1];

  protected:
    RegIndex dest;
    uint8_t sysM;

  public:
    MrsMProfile(ExtMachInst mach_inst, RegIndex _dest, uint8_t _sysM)
        : PredOp("mrs", mach_inst, IntAluOp),
          dest(_dest), sysM(_sysM)
    {
        setRegIdxArrays(
            reinterpret_cast<RegIdArrayPtr>(
                &std::remove_pointer_t<decltype(this)>::srcRegIdxArr),
            reinterpret_cast<RegIdArrayPtr>(
                &std::remove_pointer_t<decltype(this)>::destRegIdxArr));

        setDestRegIdx(_numDestRegs++, intRegClass[dest]);
        _numTypedDestRegs[intRegClass.type()]++;
    }

    Fault execute(ExecContext *xc,
                  trace::InstRecord *traceData) const override;

    std::string generateDisassembly(
            Addr pc,
            const loader::SymbolTable *symtab) const override;
};

/**
 * M-profile MSR: Move to Special Register.
 *
 * MSR <spec_reg>, Rn
 * Thumb-32 encoding T1: 111100111000 Rn[19:16] 10(0)0 SYSm[7:0]
 *
 * Same SYSm encoding as MRS (see above).
 *
 * Reference: DDI0403E A7.7.42 (MSR)
 */
class MsrMProfile : public PredOp
{
  private:
    RegId srcRegIdxArr[1];
    RegId destRegIdxArr[1];  // placeholder

  protected:
    RegIndex op1;
    uint8_t sysM;

  public:
    MsrMProfile(ExtMachInst mach_inst, RegIndex _op1, uint8_t _sysM)
        : PredOp("msr", mach_inst, IntAluOp),
          op1(_op1), sysM(_sysM)
    {
        setRegIdxArrays(
            reinterpret_cast<RegIdArrayPtr>(
                &std::remove_pointer_t<decltype(this)>::srcRegIdxArr),
            reinterpret_cast<RegIdArrayPtr>(
                &std::remove_pointer_t<decltype(this)>::destRegIdxArr));

        setSrcRegIdx(_numSrcRegs++, intRegClass[op1]);
    }

    Fault execute(ExecContext *xc,
                  trace::InstRecord *traceData) const override;

    std::string generateDisassembly(
            Addr pc,
            const loader::SymbolTable *symtab) const override;
};

/**
 * M-profile CPS: Change Processor State.
 *
 * CPSID i / CPSIE i — set/clear PRIMASK
 * CPSID f / CPSIE f — set/clear FAULTMASK (ARMv7-M only)
 *
 * Thumb-16 encoding T1: 10110110011 im[4] (0)(0) F[1] I[0]
 *   im=1: disable (set mask)
 *   im=0: enable (clear mask)
 *   I: affects PRIMASK
 *   F: affects FAULTMASK
 *
 * M-profile CPS does NOT change mode bits (no CPSR.mode on M-profile).
 *
 * Reference: DDI0403E A7.7.17 (CPS), B5.2.3 (PRIMASK), B5.2.6 (FAULTMASK)
 */
class CpsMProfile : public PredOp
{
  protected:
    bool disable;  // true = CPSID (set mask), false = CPSIE (clear mask)
    bool affectI;  // true = modify PRIMASK
    bool affectF;  // true = modify FAULTMASK

  public:
    CpsMProfile(ExtMachInst mach_inst,
                bool _disable, bool _affectI, bool _affectF)
        : PredOp("cps", mach_inst, IntAluOp),
          disable(_disable), affectI(_affectI), affectF(_affectF)
    {}

    Fault execute(ExecContext *xc,
                  trace::InstRecord *traceData) const override;

    std::string generateDisassembly(
            Addr pc,
            const loader::SymbolTable *symtab) const override;
};

/**
 * M-profile BX: Branch and Exchange with EXC_RETURN detection.
 *
 * BX Rm
 * Thumb-16 encoding T1: 01000111 0 Rm[6:3] (000)
 *
 * If the target address matches the EXC_RETURN pattern
 * (addr & 0xFFFFFFF0) == 0xFFFFFFF0, this is NOT a branch — it
 * triggers M-profile exception return (unstacking).
 *
 * EXC_RETURN values (DDI0403E B1.5.8):
 *   0xFFFFFFF1 — Return to Handler mode, use MSP
 *   0xFFFFFFF9 — Return to Thread mode, use MSP
 *   0xFFFFFFFD — Return to Thread mode, use PSP
 *
 * If not EXC_RETURN: normal branch to Rm.
 * Bit[0] of target sets Thumb state (always 1 on M-profile).
 *
 * Reference: DDI0403E A7.7.20 (BX), B1.5.8 (exception return)
 */
class BxMProfile : public PredOp
{
  private:
    RegId srcRegIdxArr[1];
    RegId destRegIdxArr[1];  // placeholder

  protected:
    RegIndex op1;

  public:
    BxMProfile(ExtMachInst mach_inst, RegIndex _op1)
        : PredOp("bx", mach_inst, IntAluOp),
          op1(_op1)
    {
        setRegIdxArrays(
            reinterpret_cast<RegIdArrayPtr>(
                &std::remove_pointer_t<decltype(this)>::srcRegIdxArr),
            reinterpret_cast<RegIdArrayPtr>(
                &std::remove_pointer_t<decltype(this)>::destRegIdxArr));

        setSrcRegIdx(_numSrcRegs++, intRegClass[op1]);

        flags[IsIndirectControl] = true;
        flags[IsUncondControl] = true;
    }

    Fault execute(ExecContext *xc,
                  trace::InstRecord *traceData) const override;

    std::string generateDisassembly(
            Addr pc,
            const loader::SymbolTable *symtab) const override;
};

/**
 * M-profile BLX reg: Branch with Link and Exchange + EXC_RETURN.
 *
 * BLX Rm
 * Thumb-16 encoding T1: 01000111 1 Rm[6:3] (000)
 *
 * Same EXC_RETURN detection as BX.  Additionally sets LR to the
 * return address (next instruction after BLX).
 *
 * Reference: DDI0403E A7.7.19 (BLX register)
 */
class BlxRegMProfile : public PredOp
{
  private:
    RegId srcRegIdxArr[1];
    RegId destRegIdxArr[1];

  protected:
    RegIndex op1;

  public:
    BlxRegMProfile(ExtMachInst mach_inst, RegIndex _op1)
        : PredOp("blx", mach_inst, IntAluOp),
          op1(_op1)
    {
        setRegIdxArrays(
            reinterpret_cast<RegIdArrayPtr>(
                &std::remove_pointer_t<decltype(this)>::srcRegIdxArr),
            reinterpret_cast<RegIdArrayPtr>(
                &std::remove_pointer_t<decltype(this)>::destRegIdxArr));

        setSrcRegIdx(_numSrcRegs++, intRegClass[op1]);
        setDestRegIdx(_numDestRegs++, intRegClass[int_reg::Lr]);
        _numTypedDestRegs[intRegClass.type()]++;

        flags[IsIndirectControl] = true;
        flags[IsUncondControl] = true;
        flags[IsCall] = true;
    }

    Fault execute(ExecContext *xc,
                  trace::InstRecord *traceData) const override;

    std::string generateDisassembly(
            Addr pc,
            const loader::SymbolTable *symtab) const override;
};

/**
 * M-profile SVC: Supervisor Call.
 *
 * SVC #imm8
 * Thumb-16 encoding T1: 11011111 imm[7:0]
 *
 * Same encoding as A-profile SVC, but generates ArmMFault(MPEXC_SVCALL)
 * instead of A-profile SupervisorCall fault.
 *
 * Reference: DDI0403E A7.7.175 (SVC), B1.5.2 (exception entry)
 */
class SvcMProfile : public PredOp
{
  protected:
    uint8_t svcImm;

  public:
    SvcMProfile(ExtMachInst mach_inst, uint8_t _imm)
        : PredOp("svc", mach_inst, IntAluOp),
          svcImm(_imm)
    {
        flags[IsSyscall] = true;
    }

    Fault execute(ExecContext *xc,
                  trace::InstRecord *traceData) const override;

    std::string generateDisassembly(
            Addr pc,
            const loader::SymbolTable *symtab) const override;
};

/**
 * M-profile WFI: Wait for Interrupt.
 *
 * M-profile WFI checks for pending unmasked interrupts via the SCS
 * (not via A-profile HCR/CPSR/SCR trap checks).  If no interrupt is
 * pending, the CPU sleeps until one arrives.
 *
 * Reference: DDI0403E A7.7.184 (WFI)
 */
class WfiMProfile : public PredOp
{
  public:
    WfiMProfile(ExtMachInst mach_inst)
        : PredOp("wfi", mach_inst, IntAluOp)
    {
        flags[IsQuiesce] = true;
    }

    Fault execute(ExecContext *xc,
                  trace::InstRecord *traceData) const override;

    std::string generateDisassembly(
            Addr pc,
            const loader::SymbolTable *symtab) const override;
};

/**
 * M-profile WFE: Wait for Event.
 *
 * M-profile WFE checks the event register.  If set, clears it and
 * returns.  If not set, sleeps until an event (SEV from another
 * core, interrupt, or debug event).
 *
 * Reference: DDI0403E A7.7.183 (WFE)
 */
class WfeMProfile : public PredOp
{
  public:
    WfeMProfile(ExtMachInst mach_inst)
        : PredOp("wfe", mach_inst, IntAluOp)
    {
        flags[IsQuiesce] = true;
    }

    Fault execute(ExecContext *xc,
                  trace::InstRecord *traceData) const override;

    std::string generateDisassembly(
            Addr pc,
            const loader::SymbolTable *symtab) const override;
};

/**
 * M-profile undefined instruction.
 *
 * Used for A-profile-only instructions that are undefined on M-profile:
 * SMC, HVC, ERET, BXJ, SRS, RFE, SUBS PC LR, MRS/MSR SPSR,
 * MRS/MSR Banked, SETEND, MRC/MCR CP14/CP15.
 *
 * Returns ArmMFault(MPEXC_USAGEFAULT) — the M-profile equivalent of
 * an undefined instruction exception.
 *
 * Reference: DDI0403E B1.5.3 (UsageFault), B3.2.15 (CFSR.UNDEFINSTR)
 */
class MProfileUndefined : public PredOp
{
  protected:
    const char *reason;

  public:
    MProfileUndefined(ExtMachInst mach_inst, const char *_reason)
        : PredOp("undefined", mach_inst, IntAluOp),
          reason(_reason)
    {
        flags[IsInvalid] = true;
    }

    Fault execute(ExecContext *xc,
                  trace::InstRecord *traceData) const override;

    std::string generateDisassembly(
            Addr pc,
            const loader::SymbolTable *symtab) const override;
};

/**
 * M-profile exception return triggered by POP {PC} / LDM {PC}.
 *
 * When a load instruction writes an EXC_RETURN value to PC, the
 * CPU tries to fetch from that address (0xFFFFFFF_).  MDecoder
 * detects this in moreBytes() and returns this synthetic instruction
 * instead.  Its execute() calls mProfileExcReturn() to perform the
 * actual exception return (unstacking).
 *
 * This handles the common pattern in Cortex-M handlers:
 *   PUSH {r4-r7, lr}    ; save callee-saved + EXC_RETURN
 *   ...handler code...
 *   POP  {r4-r7, pc}    ; restore + return via EXC_RETURN
 *
 * Reference: DDI0403E B1.5.8 (exception return)
 */
class ExcReturnFromPC : public PredOp
{
  protected:
    uint32_t excReturnVal;

  public:
    ExcReturnFromPC(ExtMachInst mach_inst, uint32_t _excReturn)
        : PredOp("exc_return", mach_inst, IntAluOp),
          excReturnVal(_excReturn)
    {}

    Fault execute(ExecContext *xc,
                  trace::InstRecord *traceData) const override;

    std::string generateDisassembly(
            Addr pc,
            const loader::SymbolTable *symtab) const override;
};

/**
 * M-profile semihosting via BKPT #0xAB.
 *
 * Per the ARM semihosting specification, M-profile uses BKPT #0xAB
 * (not SVC #0xAB) as the semihosting trigger.  The semihosting ABI
 * is the same as A-profile 32-bit: R0 = operation code, R1 = param
 * block pointer.
 *
 * The actual semihosting logic lives in ArmSemihosting (which inherits
 * from BaseSemihosting).  This instruction just calls call32() on
 * the semihosting object obtained from ArmMSystem.
 *
 * Reference: ARM Semihosting Specification, Section 3.1
 */
class BkptSemiMProfile : public PredOp
{
  public:
    BkptSemiMProfile(ExtMachInst mach_inst)
        : PredOp("bkpt_semi", mach_inst, IntAluOp)
    {}

    Fault execute(ExecContext *xc,
                  trace::InstRecord *traceData) const override;

    std::string generateDisassembly(
            Addr pc,
            const loader::SymbolTable *symtab) const override;
};

/**
 * M-profile memory barrier: DMB / DSB.
 *
 * On Cortex-M4 (in-order, no data cache), DMB and DSB both just drain
 * the write buffer.  The A-profile DSB has IsSerializeAfter which
 * causes a full pipeline flush — correct for out-of-order A-profile
 * but far too expensive for M-profile's simple pipeline.
 *
 * This class uses IsReadBarrier + IsWriteBarrier (memory ordering)
 * WITHOUT IsSerializeAfter (no pipeline flush).
 *
 * ISB is NOT intercepted — the A-profile ISB with IsSquashAfter is
 * correct for M-profile (pipeline flush is required for ISB on all
 * profiles).
 *
 * Reference: DDI0403E A7.7.27 (DMB), A7.7.28 (DSB)
 */
class BarrierMProfile : public PredOp
{
  public:
    BarrierMProfile(const char *mnem, ExtMachInst mach_inst)
        : PredOp(mnem, mach_inst, IntAluOp)
    {
        flags[IsReadBarrier] = true;
        flags[IsWriteBarrier] = true;
        // Intentionally NO IsSerializeAfter — M-profile DSB/DMB
        // should not flush the pipeline on a simple in-order core.
    }

    Fault execute(ExecContext *xc,
                  trace::InstRecord *traceData) const override;

    std::string generateDisassembly(
            Addr pc,
            const loader::SymbolTable *symtab) const override;
};

/**
 * M-profile LDREX / LDREXB / LDREXH: Load Register Exclusive.
 *
 * The shared ISA-generated LDREX instruction class calls
 * ArmISA::ISA::getSelfDebug() which performs a static_cast<ISA*>
 * on the ISA pointer.  On M-profile the ISA is MISA (a sibling of
 * ISA, not a subclass), so the cast is undefined behavior and
 * causes a segfault.  This M-profile-specific class avoids the
 * SelfDebug call entirely — M-profile has no A-profile-style
 * single-step debug extension.
 *
 * The size parameter selects the access width:
 *   4 = LDREX  (word)     DDI0403E A7.7.49
 *   2 = LDREXH (halfword) DDI0403E A7.7.51
 *   1 = LDREXB (byte)     DDI0403E A7.7.50
 *
 * Reference: DDI0403E A3.4.5 (Exclusive monitors)
 */
class LdrexMProfile : public PredOp
{
  private:
    RegId srcRegIdxArr[1];
    RegId destRegIdxArr[1];

  protected:
    RegIndex dest;     // Rt
    RegIndex base;     // Rn
    uint32_t imm;      // offset in bytes (already shifted for LDREX)
    unsigned accessSize; // 1, 2, or 4

  public:
    LdrexMProfile(ExtMachInst mach_inst, RegIndex _dest, RegIndex _base,
                  uint32_t _imm, unsigned _size)
        : PredOp("ldrex", mach_inst, MemReadOp),
          dest(_dest), base(_base), imm(_imm), accessSize(_size)
    {
        setRegIdxArrays(
            reinterpret_cast<RegIdArrayPtr>(
                &std::remove_pointer_t<decltype(this)>::srcRegIdxArr),
            reinterpret_cast<RegIdArrayPtr>(
                &std::remove_pointer_t<decltype(this)>::destRegIdxArr));

        setSrcRegIdx(_numSrcRegs++, intRegClass[base]);
        setDestRegIdx(_numDestRegs++, intRegClass[dest]);
        _numTypedDestRegs[intRegClass.type()]++;

        flags[IsLoad] = true;
    }

    Fault execute(ExecContext *xc,
                  trace::InstRecord *traceData) const override;

    // Timing-CPU split path. Without these the default StaticInst
    // implementations panic with "initiateAcc not defined!" on
    // MinorCPU. Mirror the read side of the auto-generated A-profile
    // LDREX class.
    Fault initiateAcc(ExecContext *xc,
                      trace::InstRecord *traceData) const override;
    Fault completeAcc(PacketPtr pkt, ExecContext *xc,
                      trace::InstRecord *traceData) const override;

    std::string generateDisassembly(
            Addr pc,
            const loader::SymbolTable *symtab) const override;
};

/**
 * M-profile STREX / STREXB / STREXH: Store Register Exclusive.
 *
 * Companion to LdrexMProfile. The auto-generated A-profile STREX
 * class is missing initiateAcc, so any timing-CPU run that issues
 * STREX panics with "initiateAcc not defined!". This M-profile-
 * specific class implements the exclusive-store path directly via
 * writeMemAtomicLE with Request::LLSC, mirroring the read side.
 *
 * Operands:
 *   Rd  (result)  — set to 0 on success, 1 on failure
 *   Rt  (value)   — value to store
 *   Rn  (base)    — pointer base
 *   imm           — byte offset (only the word form takes a non-zero
 *                   imm; STREXB/STREXH have imm = 0)
 *
 * accessSize selects the access width:
 *   4 = STREX  (word)     DDI0403E A7.7.221
 *   2 = STREXH (halfword) DDI0403E A7.7.223
 *   1 = STREXB (byte)     DDI0403E A7.7.222
 *
 * Reference: DDI0403E A3.4.5 (Exclusive monitors)
 */
class StrexMProfile : public PredOp
{
  private:
    RegId srcRegIdxArr[2];
    RegId destRegIdxArr[1];

  protected:
    RegIndex result_reg;  // Rd — receives 0/1 success flag
    RegIndex src;         // Rt — value to store
    RegIndex base;        // Rn — address base
    uint32_t imm;         // byte offset
    unsigned accessSize;  // 1, 2, or 4

  public:
    StrexMProfile(ExtMachInst mach_inst, RegIndex _result, RegIndex _src,
                  RegIndex _base, uint32_t _imm, unsigned _size)
        : PredOp("strex", mach_inst, MemWriteOp),
          result_reg(_result), src(_src), base(_base),
          imm(_imm), accessSize(_size)
    {
        setRegIdxArrays(
            reinterpret_cast<RegIdArrayPtr>(
                &std::remove_pointer_t<decltype(this)>::srcRegIdxArr),
            reinterpret_cast<RegIdArrayPtr>(
                &std::remove_pointer_t<decltype(this)>::destRegIdxArr));

        // Two source registers: the base address (Rn) and the data
        // value (Rt). The destination is the success/fail flag (Rd).
        setSrcRegIdx(_numSrcRegs++, intRegClass[base]);
        setSrcRegIdx(_numSrcRegs++, intRegClass[src]);
        setDestRegIdx(_numDestRegs++, intRegClass[result_reg]);
        _numTypedDestRegs[intRegClass.type()]++;

        flags[IsStore] = true;
    }

    Fault execute(ExecContext *xc,
                  trace::InstRecord *traceData) const override;

    // Timing-CPU split path. Mirror the write side of the auto-
    // generated A-profile STREX class.
    Fault initiateAcc(ExecContext *xc,
                      trace::InstRecord *traceData) const override;
    Fault completeAcc(PacketPtr pkt, ExecContext *xc,
                      trace::InstRecord *traceData) const override;

    std::string generateDisassembly(
            Addr pc,
            const loader::SymbolTable *symtab) const override;
};

/**
 * M-profile PUSH/POP: Multi-register transfer without 8B pairing.
 *
 * The Cortex-M4 has a 32-bit AHB bus — each register transfer is
 * 4 bytes, 1 cycle per beat [DDI0439D Table 3-1: LDM/STM = 1+N].
 * The A-profile MacroMemOp pairs loads into 8-byte ldr2_uop micro-ops,
 * which is correct for 64-bit buses but wrong for M-profile.
 *
 * This class creates a MacroMemOp with noPairedLoads=true, forcing
 * all loads to be single 4-byte MicroLdrUop.
 *
 * Handles:
 *   PUSH {reglist}       — STMDB SP!, {reglist}
 *   POP  {reglist}       — LDMIA SP!, {reglist}
 *   STM/LDM with SP      — 32-bit Thumb T1/T2 encodings
 *
 * Reference: DDI0403E A7.7.99 (POP), A7.7.101 (PUSH)
 */
class PushPopMProfile : public MacroMemOp
{
  public:
    PushPopMProfile(const char *mnem, ExtMachInst machInst,
                    bool load, uint32_t reglist)
        : MacroMemOp(mnem, machInst,
                     load ? MemReadOp : MemWriteOp,
                     int_reg::Sp,  // rn = SP
                     load,         // index: false for PUSH (STMDB),
                                   //        true for POP (LDMIA)
                                   // (matches A-profile data.isa:1217/1268)
                     load,         // up: false for PUSH, true for POP
                     false,        // user = false (no user mode on M-profile)
                     true,         // writeback = always (SP updated)
                     load,         // load
                     reglist,
                     true)         // noPairedLoads = true
    {
        // "LDM and STM cannot be pipelined with preceding or following
        // instructions." [DDI0439D §3.3.2]
        // Set IsSerializeAfter on the last micro-op to force a pipeline
        // flush after the transfer completes.  This models the pipeline
        // refill cost (fetch from Flash + decode + execute restart).
        // assert(numMicroops > 0);
        // microOps[numMicroops - 1]->setFlag(StaticInst::IsSerializeAfter);
    }
};

} // namespace ArmISA
} // namespace gem5

#endif // __ARCH_ARM_M_INSTS_HH__
