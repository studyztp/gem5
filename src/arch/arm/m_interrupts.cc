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
#include "arch/arm/m_system.hh"
#include "arch/arm/regs/misc.hh"
#include "arch/arm/regs/misc_types.hh"
#include "cpu/thread_context.hh"
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
    pendingSignal = false;
}

void
MProfileInterrupts::clearAll()
{
    pendingSignal = false;
    lastAckedExcNum = -1;
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

    // Ask the SCS which exception won priority resolution.
    // Store it so updateIntrInfo() can tell the SCS to activate it.
    lastAckedExcNum = scs->acknowledgeIRQ();
    return std::make_shared<ArmMFault>(lastAckedExcNum);
}

void
MProfileInterrupts::updateIntrInfo()
{
    // Tell the SCS to transition the interrupt from pending to active.
    // This clears the pending bit and sets the active bit, which
    // affects priority resolution for nested interrupts.
    if (scs && lastAckedExcNum >= 0) {
        scs->activateIRQ(lastAckedExcNum);
        lastAckedExcNum = -1;
    }
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
//   2. Unstack:    restore CPU state via mProfileExcReturnUnstack()

void
MProfileInterrupts::excReturn(ThreadContext *tc, uint32_t exc_return)
{
    // Read current IPSR to identify which exception is returning.
    // Must happen before unstacking, which restores the stacked xPSR
    // (overwriting IPSR with the interrupted context's exception number).
    XPSR currentXpsr = tc->readMiscRegNoEffect(MISCREG_M_XPSR);
    int returningExc = currentXpsr.exception;

    // Deactivate the returning exception — clear its active bit in
    // NVIC (nvicActive[]) or SHCSR.
    // DDI0403E B1.5.8: exception return transitions the returning
    // exception from active to inactive.
    if (returningExc > 0 && scs) {
        scs->deactivateIRQ(returningExc);
    }

    // Unstack the exception frame and restore CPU state.
    // This restores r0-r3, r12, LR, SP, xPSR, CONTROL.SPSEL,
    // and sets NPC to the stacked return address.
    mProfileExcReturnUnstack(tc, exc_return);
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
