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
 * M-profile interrupt controller (CPU-side) implementation.
 *
 * On M-profile, the NVIC (inside the SCS device) owns all per-interrupt
 * state: enabled, pending, active, priority for up to 240 IRQs plus
 * system exceptions.  The NVIC performs priority resolution and then
 * signals the CPU with a single "something is deliverable" doorbell.
 *
 * This class bridges the BaseInterrupts interface to the SCS:
 *
 *   post()/clear():
 *     The int_num/index parameters are NOT per-interrupt IDs.  They are
 *     artifacts of the BaseInterrupts interface.  On M-profile, the SCS
 *     calls cpu->postInterrupt(0,0,0) as a single doorbell signal after
 *     updatePending() has already resolved priorities.  So post() just
 *     sets pendingSignal=true and clear() sets it to false.
 *
 *     IMPORTANT: pendingSignal is only an optimization hint to skip the
 *     SCS query when we know nothing was posted.  The SCS only calls
 *     clearFromInterruptController() from updatePending() when NO
 *     interrupt is deliverable, so clear() setting false is safe in
 *     that context.  However, checkInterrupts() always double-checks
 *     with scs->hasDeliverableIRQ() because masks (PRIMASK, BASEPRI)
 *     can change between the post() and the check, potentially making
 *     a previously-deliverable interrupt undeliverable.
 *
 *   checkInterrupts():
 *     Quick-exit if !pendingSignal, then ask SCS for the real answer.
 *     This means pendingSignal=true does NOT guarantee delivery -- the
 *     SCS re-evaluates masks and priorities on every query.
 *
 *   getInterrupt():
 *     Asks SCS for the exception number, wraps it in ArmMFault.
 *
 *   updateIntrInfo():
 *     Tells SCS to transition the interrupt pending->active.
 */

#include "arch/arm/m_interrupts.hh"

#include "arch/arm/m_faults.hh"
#include "arch/arm/m_mmu.hh"
#include "arch/arm/m_system.hh"
#include "arch/arm/pcstate.hh"
#include "arch/arm/regs/int.hh"
#include "arch/arm/regs/misc.hh"
#include "arch/arm/regs/misc_types.hh"
#include "base/trace.hh"
#include "cpu/base.hh"
#include "cpu/thread_context.hh"
#include "debug/Faults.hh"
#include "debug/MProfileStacking.hh"
#include "dev/arm/m_profile_scs.hh"

namespace gem5
{

namespace ArmISA
{

// M-profile BitUnion types (XPSR, etc.) live inside namespace ArmMISA
// (declared in misc_types.hh).  The using-directive makes them
// available without the ArmMISA:: prefix.
using namespace ArmMISA;

MProfileInterrupts::MProfileInterrupts(const Params &p)
    : BaseInterrupts(p)
{}

void
MProfileInterrupts::setThreadContext(ThreadContext *_tc)
{
    BaseInterrupts::setThreadContext(_tc);

    // Discover the SCS/NVIC device through the system object.
    // MProfileInterrupts has no explicit SCS param -- it finds the
    // SCS via ArmMSystem, which registers the SCS during init().
    auto *msys = dynamic_cast<ArmMSystem *>(_tc->getSystemPtr());
    if (msys)
        scs = msys->getSCS();
}

// =========================================================================
// Signal interface (called by BaseCPU on behalf of SCS)
// =========================================================================
//
// These are doorbell signals, not per-interrupt tracking.
// The SCS has already resolved priorities before calling post().
// See file-level comment for why int_num/index are unused.

void
MProfileInterrupts::post(int int_num, int index)
{
    pendingSignal = true;
}

void
MProfileInterrupts::clear(int int_num, int index)
{
    pendingSignal = scs->hasDeliverableIRQ();
}

void
MProfileInterrupts::clearAll()
{
    pendingSignal = scs->hasDeliverableIRQ();
    lastAckedExcNum = -1;
    stackOperating = false;
    stackPriority = 256;
    returningExcNum = -1;
}


// MMU will call to notify when stack read/write finishes

void
MProfileInterrupts::setupStackPending()
{
    stackOperating = true;
    stackPriority = scs->getCurrentExcPriority();
    returningExcNum = scs->getCurrentExcNum();
}

void
MProfileInterrupts::removeStackReadPending()
{
    DPRINTF(MProfileStacking,
            "removeStackReadPending: stackOperating=%d "
            "returningExcNum=%d\n",
            stackOperating, returningExcNum);

    assert(returningExcNum > 0);
    scs->deactivateIRQ(returningExcNum);
    returningExcNum = -1;

    stackOperating = false;
    stackPriority = 256;
}

void
MProfileInterrupts::removeStackWritePending()
{
    DPRINTF(MProfileStacking,
            "removeStackWritePending: stackOperating=%d "
            "returningExcNum=%d\n",
            stackOperating, returningExcNum);

    assert(returningExcNum > 0);
    returningExcNum = -1;

    stackOperating = false;
    stackPriority = 256;
}


// =========================================================================
// CPU pipeline interface
// =========================================================================

bool
MProfileInterrupts::checkInterrupts() const
{
    // Quick exit: if the SCS hasn't posted anything, no need to query.
    if (!pendingSignal || !scs)
        return false;

    // Even though pendingSignal is true, the interrupt may no longer
    // be deliverable.  Masks (PRIMASK, BASEPRI, FAULTMASK) can change
    // between when the SCS posted and when the CPU checks.  The SCS
    // re-evaluates the full priority/mask state on every call.
    return scs->hasDeliverableIRQ();
}

Fault
MProfileInterrupts::getInterrupt()
{
    assert(scs);

    if (stackOperating) {
        // Stacking or unstacking is in progress.  Only allow a
        // strictly higher priority exception to preempt.
        // Same or lower priority must wait until the operation
        // completes (DDI0403E B1.5.14).
        int pendingExc = scs->acknowledgeIRQ();
        int16_t pendingPri = scs->getExcPriority(pendingExc);

        DPRINTF(MProfileStacking,
                "getInterrupt: stackOperating=true, pendingExc=%d "
                "pendingPri=%d stackPriority=%d\n",
                pendingExc, pendingPri, stackPriority);

        if (pendingPri >= stackPriority) {
            return NoFault;
        }

        // Higher priority: allow preemption, abandon unstacking.
        DPRINTF(MProfileStacking,
                "getInterrupt: preempting with higher-priority "
                "exc#%d (pri=%d > stack pri=%d)\n",
                pendingExc, pendingPri, stackPriority);

        auto *mmu = dynamic_cast<MMMU *>(tc->getMMUPtr());
        assert(mmu);
        mmu->abandonUnstacking();
    }

    // Ask the SCS which exception won priority resolution.
    lastAckedExcNum = scs->acknowledgeIRQ();
    return std::make_shared<ArmMFault>(lastAckedExcNum);
}

void
MProfileInterrupts::updateIntrInfo()
{
    // Activation is now handled by ArmMFault::invoke() via
    // scs->activateIRQ().  Nothing to do here — just clear
    // the acked exception number.
    lastAckedExcNum = -1;
}

bool
MProfileInterrupts::validateExcReturn(ThreadContext *tc, uint32_t exc_return,
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
MProfileInterrupts::excReturnUnstack(ThreadContext *tc, uint32_t exc_return)
{
    DPRINTFS(MProfileStacking, tc->getCpuPtr(),
             "excReturnUnstack: exc_return=%#x pc=%#x\n",
             exc_return, tc->pcState().as<PCState>().pc());

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
    DPRINTFS(MProfileStacking, tc->getCpuPtr(),
             "excReturnUnstack: calling readFromStack "
             "frameptr=%#x restoreFromPSP=%d\n",
             frameptr, restoreFromPSP);
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

// =========================================================================
// Exception return (called when CPU detects EXC_RETURN address)
// =========================================================================
//
// M-profile exception return is hardware-automatic: the CPU detects
// the EXC_RETURN value, deactivates the returning exception, and
// unstacks the exception frame — all in one operation.
// This is unlike A-profile where software explicitly writes to
// ICC_EOIR to deactivate.
//
// Split into two phases:
//   1. Deactivate: clear active bit via scs->deactivateIRQ()
//   2. Unstack:    restore CPU state via excReturnUnstack()

void
MProfileInterrupts::excReturn(ThreadContext *tc, uint32_t exc_return)
{
    // Read current IPSR to identify which exception is returning.
    // Must happen before unstacking, which restores the stacked xPSR
    // (overwriting IPSR with the interrupted context's exception number).
    XPSR currentXpsr = tc->readMiscRegNoEffect(MISCREG_M_XPSR);
    returningExcNum = currentXpsr.exception;

    DPRINTF(MProfileStacking,
            "excReturn: exc_return=%#x returningExcNum=%d\n",
            exc_return, returningExcNum);

    // Do NOT deactivate here — the exception stays active during
    // unstacking.  Deactivation happens in removeStackReadPending()
    // when the MMU signals that all unstacking reads have completed.
    // This matches real HW: deactivation occurs when the return
    // sequence completes successfully (DDI0403E B1.5.8).

    // Unstack the exception frame and restore CPU state.
    // readFromStack() inside excReturnUnstack() will call
    // setupStackReadPending() and register the completion callback.
    DPRINTF(MProfileStacking,
            "excReturn: calling excReturnUnstack\n");
    excReturnUnstack(tc, exc_return);
}

// =========================================================================
// Serialization (minimal for MVP)
// =========================================================================

void
MProfileInterrupts::serialize(CheckpointOut &cp) const
{
    SERIALIZE_SCALAR(pendingSignal);
    SERIALIZE_SCALAR(lastAckedExcNum);
}

void
MProfileInterrupts::unserialize(CheckpointIn &cp)
{
    UNSERIALIZE_SCALAR(pendingSignal);
    UNSERIALIZE_SCALAR(lastAckedExcNum);
}

} // namespace ArmISA
} // namespace gem5
