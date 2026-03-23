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

#include "arch/arm/m_faults.hh"

#include "arch/arm/m_mmu.hh"
#include "arch/arm/m_system.hh"
#include "arch/arm/pcstate.hh"
#include "arch/arm/regs/cc.hh"
#include "arch/arm/regs/int.hh"
#include "arch/arm/regs/misc.hh"
#include "arch/arm/regs/misc_types.hh"
#include "arch/arm/system.hh"
#include "base/trace.hh"
#include "cpu/base.hh"
#include "cpu/static_inst.hh"
#include "cpu/thread_context.hh"
#include "debug/Faults.hh"
#include "dev/arm/m_profile_scs.hh"
#include "mem/port_proxy.hh"
#include "sim/full_system.hh"

namespace gem5
{

namespace ArmISA
{

// M-profile BitUnion types (XPSR, CCR_t, CONTROL_M, …) live inside
// namespace ArmMISA (declared in misc_types.hh).  The using-directive
// makes them available without the ArmMISA:: prefix in all the
// M-profile fault code below.
using namespace ArmMISA;

// ---------------------------------------------------------------------------
// Static helpers
// ---------------------------------------------------------------------------

const char *
ArmMFault::excName(int exc_num)
{
    switch (exc_num) {
      case MPEXC_RESET:      return "M-Profile Reset";
      case MPEXC_NMI:        return "M-Profile NMI";
      case MPEXC_HARDFAULT:  return "M-Profile HardFault";
      case MPEXC_MEMMANAGE:  return "M-Profile MemManage";
      case MPEXC_BUSFAULT:   return "M-Profile BusFault";
      case MPEXC_USAGEFAULT: return "M-Profile UsageFault";
      case MPEXC_SVCALL:     return "M-Profile SVCall";
      case MPEXC_DEBUGMON:   return "M-Profile DebugMon";
      case MPEXC_PENDSV:     return "M-Profile PendSV";
      case MPEXC_SYSTICK:    return "M-Profile SysTick";
      default:
        if (exc_num >= MPEXC_EXTERNAL_BASE)
            return "M-Profile IRQ";
        return "M-Profile Unknown";
    }
}

bool
ArmMFault::excAdvancesPC(int exc_num)
{
    // Only SVCall advances the return address past the current
    // instruction.  All other exceptions either retry the faulting
    // instruction (synchronous faults) or were taken between
    // instructions (asynchronous — PC already at the next instruction).
    return exc_num == MPEXC_SVCALL;
}

FaultName
ArmMFault::name() const
{
    return excName(_excNumber);
}

// ---------------------------------------------------------------------------
// Exception entry helpers
// ---------------------------------------------------------------------------

Addr
ArmMFault::getHandlerAddress(ThreadContext *tc) const
{
    uint32_t vtor = tc->readMiscRegNoEffect(MISCREG_M_VTOR);
    PortProxy &phys = tc->getSystemPtr()->physProxy;
    return phys.read<uint32_t>(vtor + 4 * _excNumber, ByteOrder::little);
}

uint32_t
ArmMFault::computeExcReturn(bool was_handler, bool used_psp)
{
    // EXC_RETURN encoding (non-FP, DDI0403E B1.5.8):
    //   bits[31:4] = 0xFFFFFFF
    //   bit[3] = 0: return to Handler mode, 1: return to Thread mode
    //   bit[2] = 0: restore from MSP,       1: restore from PSP
    //   bits[1:0] = 0b01 (reserved, always 1)
    if (was_handler)
        return 0xFFFFFFF1;  // Handler, MSP
    else if (used_psp)
        return 0xFFFFFFFD;  // Thread, PSP
    else
        return 0xFFFFFFF9;  // Thread, MSP
}

// =========================================================================
// CC flat register ↔ xPSR NZCV/GE sync helpers (BUG-5)
// =========================================================================

void
syncCCRegsToXpsr(ThreadContext *tc)
{
    // Read current xPSR and overwrite NZCV + GE from CC flat regs.
    // CC flat regs are authoritative — xPSR's NZCV/GE bits are stale
    // because A-profile ALU instructions only update CC regs.
    XPSR xpsr = tc->readMiscRegNoEffect(MISCREG_M_XPSR);

    // cc_reg::Nz stores N and Z as a 2-bit value: bit[1]=N, bit[0]=Z
    RegVal ccNz = tc->getReg(cc_reg::Nz);
    xpsr.n  = bits(ccNz, 1);            // xPSR[31] = N
    xpsr.z  = bits(ccNz, 0);            // xPSR[30] = Z
    xpsr.c  = tc->getReg(cc_reg::C);    // xPSR[29] = C
    xpsr.v  = tc->getReg(cc_reg::V);    // xPSR[28] = V
    xpsr.ge = tc->getReg(cc_reg::Ge);   // xPSR[19:16] = GE[3:0]

    tc->setMiscRegNoEffect(MISCREG_M_XPSR, xpsr);
}

void
syncXpsrToCCRegs(ThreadContext *tc)
{
    // Read xPSR and push NZCV + GE into CC flat regs.
    // Used after xPSR is written (MSR APSR, exception return unstack,
    // unserialize) so that subsequent conditional instructions see
    // the correct flags.
    XPSR xpsr = tc->readMiscRegNoEffect(MISCREG_M_XPSR);

    // Pack N and Z into the 2-bit Nz format: bit[1]=N, bit[0]=Z
    RegVal ccNz = ((uint64_t)xpsr.n << 1) | (uint64_t)xpsr.z;
    tc->setReg(cc_reg::Nz, ccNz);
    tc->setReg(cc_reg::C,  (RegVal)xpsr.c);
    tc->setReg(cc_reg::V,  (RegVal)xpsr.v);
    tc->setReg(cc_reg::Ge, (RegVal)xpsr.ge);
}

uint32_t
ArmMFault::pushExceptionFrame(ThreadContext *tc,
                              const StaticInstPtr &inst,
                              uint32_t sp, bool stkalign)
{
    auto pcState = tc->pcState().as<PCState>();

    // Compute return address.
    // SVCall: advance past the SVC instruction so it is not re-executed.
    // Faults: return to the faulting instruction (for retry).
    // Async:  PC already points to the next instruction to execute.
    Addr returnAddr = pcState.pc();
    if (excAdvancesPC(_excNumber) && inst != nullStaticInstPtr)
        returnAddr += inst->size();

    // Build the stacked xPSR.
    //
    // IMPORTANT: CC flat register → xPSR sync.
    //
    // gem5 reuses A-profile instruction implementations for M-profile.
    // These instructions store NZCV and GE condition flags in flat CC
    // registers (cc_reg::Nz, C, V, Ge) rather than in MISCREG_M_XPSR.
    // During normal execution this is fine — instructions read/write
    // the CC regs directly and never consult xPSR.
    //
    // But when we stack xPSR for an exception frame, the NZCV/GE bits
    // in MISCREG_M_XPSR are STALE (last written by MSR, not by the
    // most recent ALU instruction).  We must capture the live CC reg
    // values into xPSR before pushing the frame.
    //
    // On real Cortex-M hardware this isn't an issue because the ALU
    // flags and the xPSR register are the same physical bits — the
    // hardware captures them atomically when pushing the exception
    // frame.  In gem5, the CC regs and xPSR are separate storage, so
    // we must sync explicitly.
    //
    // This mirrors A-profile faults.cc:501-504 which syncs CC regs
    // into saved_cpsr before writing SPSR on exception entry.
    //
    // Without this sync, NZCV flags are silently corrupted across
    // any exception boundary.  The bug is latent for simple tests
    // (flags happen to survive by luck) but causes FreeRTOS to fail
    // after thousands of context switches when a PendSV fires between
    // a CMP and a conditional branch.
    // BUG-5: use shared helper to sync CC flat regs → MISCREG_M_XPSR.
    // This updates the stored xPSR with live NZCV/GE from CC regs.
    syncCCRegsToXpsr(tc);

    // Now read the freshly-synced xPSR and set the T bit from PCState.
    XPSR xpsr = tc->readMiscRegNoEffect(MISCREG_M_XPSR);
    xpsr.t  = pcState.thumb() ? 1 : 0;  // xPSR[24] = T (Thumb)

    // Stack alignment (DDI0403E B1.5.6):
    // If STKALIGN is set and SP is not 8-byte aligned, insert 4 bytes
    // of padding and record it in xPSR.frameptralign (bit 9) so that
    // exception return can undo the padding.
    bool needPad = stkalign && (sp & 0x4);
    if (needPad) {
        sp -= 4;
        xpsr.frameptralign = 1;
    } else {
        xpsr.frameptralign = 0;
    }

    // Decrement SP by 32 bytes (8 words) for the exception frame.
    uint32_t frameptr = sp - 0x20;

    // Build the 8-word frame as a vector for MMMU::storeToStack().
    // Order matches the hardware frame layout (DDI0403E B1.5.6):
    //   frameptr+0x00: R0, +0x04: R1, +0x08: R2, +0x0C: R3,
    //   +0x10: R12, +0x14: LR, +0x18: ReturnAddr, +0x1C: xPSR
    std::vector<uint32_t> frame = {
        (uint32_t)tc->getReg(int_reg::R0),
        (uint32_t)tc->getReg(int_reg::R1),
        (uint32_t)tc->getReg(int_reg::R2),
        (uint32_t)tc->getReg(int_reg::R3),
        (uint32_t)tc->getReg(int_reg::R12),
        (uint32_t)tc->getReg(int_reg::Lr),
        (uint32_t)returnAddr,
        (uint32_t)xpsr,
    };

    // Push the frame through the timed memory system via MMMU.
    // This models the Cortex-M stacking sequencer: writes go through
    // the stackbus with 1-cycle-per-word AHB pipelining, and the
    // stacking barrier holds icache responses until all writes complete.
    auto *mmu = dynamic_cast<MMMU *>(tc->getMMUPtr());
    assert(mmu && "M-profile CPU must use MMMU");
    mmu->storeToStack(frameptr, frame, tc);

    return frameptr;
}

// ---------------------------------------------------------------------------
// ArmMFault::invoke — common M-profile exception entry
// ---------------------------------------------------------------------------

void
ArmMFault::invoke(ThreadContext *tc, const StaticInstPtr &inst)
{
    if (!FullSystem)
        return;

    // ---- 0. Try to activate this exception via SCS ----
    //
    // SCS checks: enabled, masks (FAULTMASK/PRIMASK/BASEPRI),
    // and active exception priority.  If the exception can't be
    // taken, it is pended.  For synchronous exceptions that can't
    // preempt (e.g., SVCall from a handler with equal/higher
    // priority), escalate to HardFault.

    auto *msys = dynamic_cast<ArmMSystem *>(tc->getSystemPtr());
    assert(msys && "M-profile fault on non-M-profile system");
    MProfileSCS *scs = msys->getSCS();
    assert(scs && "MProfileSCS not registered with ArmMSystem");

    if (!scs->activateIRQ(_excNumber)) {
        // Exception couldn't be activated (blocked by priority/masks).
        // For synchronous exceptions, escalate to HardFault by
        // creating and invoking a new HardFault.
        // DDI0403E B1.5.4: "If the priority of the exception is the
        // same as or lower than the execution priority, the processor
        // escalates the exception to a HardFault."
        if (_excNumber != MPEXC_HARDFAULT) {
            auto hardFault = std::make_shared<ArmMFault>(MPEXC_HARDFAULT);
            hardFault->invoke(tc, inst);
            return;  // HardFault's invoke handles everything
        } else {
            // HardFault itself couldn't activate — lockup.
            fatal("M-profile lockup: HardFault blocked. "
                  "DDI0403E B1.5.15.");
        }
    }

    // ---- 1. Determine pre-exception state ----

    XPSR xpsr = tc->readMiscRegNoEffect(MISCREG_M_XPSR);
    bool inHandler = (xpsr.exception != 0);

    CONTROL_M ctrl = tc->readMiscRegNoEffect(MISCREG_M_CONTROL);
    bool usePSP = !inHandler && ctrl.spsel;

    MiscRegIndex spReg = usePSP ? MISCREG_M_PSP : MISCREG_M_MSP;
    uint32_t sp = (uint32_t)tc->getReg(int_reg::Sp);

    // ---- 2. Push exception frame via MMMU ----

    CCR_t ccr = tc->readMiscRegNoEffect(MISCREG_M_CCR);
    uint32_t newSP = pushExceptionFrame(tc, inst, sp, ccr.stkalign);

    // Update active-stack misc reg with post-push value.
    tc->setMiscRegNoEffect(spReg, newSP);

    // Handler mode always uses MSP.  When entering from Thread/PSP,
    // R13 must switch to MSP (DDI0403E B1.5.6).
    if (usePSP) {
        uint32_t msp = (uint32_t)tc->readMiscRegNoEffect(MISCREG_M_MSP);
        tc->setReg(int_reg::Sp, (RegVal)msp);
    } else {
        tc->setReg(int_reg::Sp, (RegVal)newSP);
    }

    // Clear CONTROL.SPSEL — handler always uses MSP (DDI0403E B1.4.4).
    ctrl.spsel = 0;
    tc->setMiscRegNoEffect(MISCREG_M_CONTROL, ctrl);

    // ---- 3. Set LR to EXC_RETURN ----

    uint32_t excReturn = computeExcReturn(inHandler, usePSP);
    tc->setReg(int_reg::Lr, (RegVal)excReturn);

    // ---- 4. Read handler address from VTOR vector table ----

    Addr handlerAddr = getHandlerAddress(tc);

    // ---- 5. Enter Handler mode ----

    // Re-read xPSR after pushExceptionFrame (which synced CC→xPSR).
    xpsr = tc->readMiscRegNoEffect(MISCREG_M_XPSR);

    // Set xPSR.IPSR = exception number (→ Handler mode).
    // Clear ICI/IT state (DDI0403E B1.5.6).
    xpsr.exception = _excNumber;
    xpsr.iciIt1 = 0;
    xpsr.iciIt2 = 0;
    tc->setMiscRegNoEffect(MISCREG_M_XPSR, xpsr);

    // Active bit is already set by scs->activateIRQ() above.
    // No need to write SHCSR — SCS owns all active state.

    // ---- 6. Branch to handler ----

    PCState pc(handlerAddr & ~0x1);
    pc.thumb(true);
    pc.nextThumb(true);
    pc.aarch64(false);
    pc.nextAArch64(false);
    pc.illegalExec(false);
    tc->pcState(pc);

    DPRINTF(Faults, "M-profile exception entry: %s (exc %d) "
            "handler=%#x SP=%#x LR=%#x\n",
            name(), _excNumber, handlerAddr & ~0x1, newSP, excReturn);
}

// ---------------------------------------------------------------------------
// MProfileReset::invoke — M-profile reset sequence
// ---------------------------------------------------------------------------

void
MProfileReset::invoke(ThreadContext *tc, const StaticInstPtr &inst)
{
    if (!FullSystem)
        return;

    // Clear interrupts and architectural state.
    // This calls MISA::clear() which zeros all misc regs including VTOR.
    tc->getCpuPtr()->clearInterrupts(tc->threadId());
    tc->clearArchRegs();

    // Set VTOR to the address provided by the workload.
    // On real hardware, VTOR resets to 0 and a boot alias maps flash
    // to address 0.  In gem5, the workload passes the actual flash
    // base address (e.g., 0x08000000 for STM32) since we may not
    // have a boot alias.  This must happen AFTER clearArchRegs
    // (which zeros VTOR) and BEFORE reading the vector table.
    tc->setMiscRegNoEffect(MISCREG_M_VTOR, vtorAddr);
    uint32_t vtor = vtorAddr;
    PortProxy &phys = tc->getSystemPtr()->physProxy;

    // Entry 0: initial Main Stack Pointer value.
    uint32_t initialMSP = phys.read<uint32_t>(vtor, ByteOrder::little);
    // Entry 1: Reset_Handler address.
    uint32_t resetHandler = phys.read<uint32_t>(vtor + 4,
                                                ByteOrder::little);

    // Initialise MSP and the architectural SP register (R13).
    // On real Cortex-M, MSP IS R13 when CONTROL.SPSEL=0.
    // In gem5, MISCREG_M_MSP and int_reg::Sp are separate storage,
    // so we must set both to keep them in sync.
    tc->setMiscRegNoEffect(MISCREG_M_MSP, initialMSP);
    tc->setReg(int_reg::Sp, (RegVal)initialMSP);

    // Set xPSR to reset value: T-bit set, Thread mode (IPSR = 0).
    tc->setMiscRegNoEffect(MISCREG_M_XPSR, 0x01000000);

    // Branch to Reset_Handler.
    // Bit[0] indicates Thumb state (must be 1); cleared for the address.
    // TODO: Same Thumb decoder dependency as ArmMFault::invoke() — see
    // the TODO there regarding Step 8 audit of thumb.isa coverage.
    PCState pc(resetHandler & ~0x1);
    pc.thumb(true);
    pc.nextThumb(true);
    pc.aarch64(false);
    pc.nextAArch64(false);
    pc.illegalExec(false);
    tc->pcState(pc);

    DPRINTF(Faults, "M-profile Reset: MSP=%#x handler=%#x\n",
            initialMSP, resetHandler & ~0x1);
}

// ---------------------------------------------------------------------------
// mProfileExcReturnUnstack — M-profile exception return (CPU state restore)
// ---------------------------------------------------------------------------
//
// Pops the 8-word exception frame from the stack indicated by
// EXC_RETURN and restores CPU state (r0-r3, r12, LR, SP, xPSR, NPC).
// This is the CPU-state-only half of exception return.  The interrupt
// controller half (deactivateIRQ) is handled by
// MProfileInterrupts::excReturn(), which calls this function.

// MISSING-1: Validate EXC_RETURN value.
// Returns true if valid, false if invalid (with reason string).
// DDI0403E B1.5.8: invalid EXC_RETURN should generate UsageFault (INVPC).
// Full fault generation is not implemented — the caller uses fatal_if
// to halt on invalid values, since they indicate firmware bugs (LR
// corruption) that are nearly impossible in correct code.
static bool
validateExcReturn(ThreadContext *tc, uint32_t exc_return,
                  const char *&reason)
{
    // Check 1: bits[31:4] must be 0xFFFFFFF (reserved encoding)
    if ((exc_return & 0xFFFFFFF0) != 0xFFFFFFF0) {
        reason = "reserved (bits[31:4] != 0xFFFFFFF)";
        return false;
    }

    // Check 2: Handler mode (bit[3]=0) + PSP (bit[2]=1) is impossible
    // — Handler mode always uses MSP
    if (!(exc_return & 0x8) && (exc_return & 0x4)) {
        reason = "Handler mode + PSP is invalid";
        return false;
    }

    // Check 3: return to Thread (bit[3]=1) from Thread mode (IPSR==0)
    // — can only do exception return from Handler mode
    uint32_t ipsr = bits(
        tc->readMiscRegNoEffect(MISCREG_M_XPSR), 8, 0);
    if ((exc_return & 0x8) && ipsr == 0) {
        reason = "return to Thread from Thread mode (IPSR==0)";
        return false;
    }

    return true;
}

void
mProfileExcReturnUnstack(ThreadContext *tc, uint32_t exc_return)
{
    // MISSING-1: validate EXC_RETURN — halt on invalid values.
    // Invalid EXC_RETURN indicates firmware bug (LR corruption).
    // On real hardware this would generate UsageFault (INVPC).
    const char *reason = nullptr;
    fatal_if(!validateExcReturn(tc, exc_return, reason),
             "Invalid EXC_RETURN 0x%08x: %s. "
             "This indicates a firmware bug (corrupted LR). "
             "Valid values: 0xFFFFFFF1 (Handler/MSP), "
             "0xFFFFFFF9 (Thread/MSP), 0xFFFFFFFD (Thread/PSP). "
             "(DDI0403E B1.5.8)",
             exc_return, reason);


    // Decode EXC_RETURN (DDI0403E B1.5.8).
    //   bit[3] = 0: return to Handler mode, 1: return to Thread mode
    //   bit[2] = 0: restore from MSP,       1: restore from PSP
    bool returnToThread = (exc_return & 0x8);
    bool restoreFromPSP = (exc_return & 0x4);

    MiscRegIndex spReg = restoreFromPSP ? MISCREG_M_PSP : MISCREG_M_MSP;

    // Read the frame pointer from the correct stack source.
    //
    // For MSP frames (bit[2]=0): read from R13 (int_reg::Sp).
    //   Handler mode R13 IS MSP.  Reading R13 is correct even after
    //   nested exceptions, because inner exception entries overwrite
    //   MISCREG_M_MSP (via setMiscRegNoEffect in invoke()) but the
    //   handler's PUSH/POP only track R13.  After the handler's
    //   software POP, R13 points to the hardware exception frame.
    //   MISCREG_M_MSP may still hold a stale value from an inner
    //   exception's entry — using it would pop from the wrong address.
    //
    // For PSP frames (bit[2]=1): read from MISCREG_M_PSP.
    //   Handler mode R13 = MSP ≠ PSP.  The frame was pushed to PSP
    //   before R13 switched to MSP on exception entry, so we must
    //   use the misc reg to find it.
    //
    // DDI0403E B1.5.8: SP_process / SP_main selected by EXC_RETURN bit[2].
    uint32_t frameptr;
    if (restoreFromPSP) {
        frameptr = (uint32_t)tc->readMiscRegNoEffect(MISCREG_M_PSP);
    } else {
        frameptr = (uint32_t)tc->getReg(int_reg::Sp);
    }

    // Read the exception frame via MMMU::readFromStack().
    // Phase 1 (inside readFromStack): sendFunctional() for immediate
    // data.  Phase 2: timing reads for realistic latency with the
    // stacking barrier holding icache responses until done.
    auto *mmu = dynamic_cast<MMMU *>(tc->getMMUPtr());
    assert(mmu && "M-profile CPU must use MMMU");
    std::vector<uint32_t> frame = mmu->readFromStack(frameptr, 8, tc);

    // Frame layout (DDI0403E B1.5.6):
    //   [0]=R0, [1]=R1, [2]=R2, [3]=R3,
    //   [4]=R12, [5]=LR, [6]=ReturnAddr, [7]=xPSR
    uint32_t retAddr = frame[6];
    XPSR xpsr = frame[7];

    // Restore general-purpose registers.
    tc->setReg(int_reg::R0,  (RegVal)frame[0]);
    tc->setReg(int_reg::R1,  (RegVal)frame[1]);
    tc->setReg(int_reg::R2,  (RegVal)frame[2]);
    tc->setReg(int_reg::R3,  (RegVal)frame[3]);
    tc->setReg(int_reg::R12, (RegVal)frame[4]);
    tc->setReg(int_reg::Lr,  (RegVal)frame[5]);

    // Restore SP — undo alignment padding if frameptralign was set.
    // Write both R13 (authoritative) and misc reg copy.
    uint32_t sp = frameptr + 0x20;
    if (xpsr.frameptralign)
        sp += 4;
    tc->setReg(int_reg::Sp, (RegVal)sp);
    tc->setMiscRegNoEffect(spReg, sp);

    // Bug 4 fix: restore CONTROL.SPSEL from EXC_RETURN bit[2].
    // Returning to Thread/PSP (bit[2]=1) → SPSEL=1.
    // Returning to Thread/MSP or Handler (bit[2]=0) → SPSEL=0.
    // DDI0403E B1.4.4: CONTROL.SPSEL is only meaningful in Thread mode
    // and is restored by exception return.
    {
        CONTROL_M ctrl = tc->readMiscRegNoEffect(MISCREG_M_CONTROL);
        ctrl.spsel = restoreFromPSP ? 1 : 0;
        tc->setMiscRegNoEffect(MISCREG_M_CONTROL, ctrl);
    }

    // Restore xPSR.  The stacked value already contains the correct
    // IPSR for the interrupted context (0 for Thread mode, non-zero
    // for a nested Handler).  Clear frameptralign since it is only
    // meaningful in the stacked copy.
    xpsr.frameptralign = 0;
    tc->setMiscRegNoEffect(MISCREG_M_XPSR, xpsr);

    // BUG-5: use shared helper to sync xPSR → CC flat regs.
    // The stacked xPSR has the interrupted code's NZCV/GE flags.
    // After restoring xPSR above, push those flags into CC flat regs
    // so conditional instructions in the resumed code see correct values.
    syncXpsrToCCRegs(tc);

    // Set NPC to the stacked return address.
    //
    // mProfileExcReturn() is called from execute() (via BxMProfile or
    // ExcReturnFromPC), so advancePC() will call advance() afterwards:
    //   advance(): _pc = _npc; _npc = _pc + instSize
    // We must therefore set _npc = retAddr, NOT use the PCState(addr)
    // constructor (which calls set() → _npc = addr+2 → off by two).
    //
    // Contrast with ArmMFault::invoke() / MProfileReset::invoke(): those
    // are true fault handlers where the fault path does NOT call advance()
    // after tc->pcState(pc), so PCState(addr) is correct there.
    //
    // Here we take the current pcState and only change _npc and the
    // Thumb-state flag.  _pc, _aarch64, etc. are already correct since we
    // are mid-execution on an M-profile (Thumb, AArch32) instruction.
    auto pc = tc->pcState().as<PCState>();
    pc.nextThumb(true);                    // M-profile always returns to Thumb
    pc.illegalExec(false);
    // NPC = return address; advance() copies to PC.
    pc.npc(retAddr & ~(Addr)0x1);
    tc->pcState(pc);

    DPRINTF(Faults, "M-profile exception return: EXC_RETURN=%#x "
            "retAddr=%#x SP=%#x %s mode\n",
            exc_return, retAddr & ~0x1, sp,
            returnToThread ? "Thread" : "Handler");
}

} // namespace ArmISA
} // namespace gem5
