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

#ifndef __ARCH_ARM_M_INTERRUPTS_HH__
#define __ARCH_ARM_M_INTERRUPTS_HH__

/**
 * @file
 * M-profile (ARMv7-M / ARMv8-M) interrupt controller interface.
 *
 * On A-profile, ArmInterrupts checks CPSR.I/F, HCR_EL2, SCR_EL3 to
 * decide if IRQ/FIQ/ABT are deliverable.  M-profile has none of those.
 * Instead, interrupt masking uses PRIMASK, BASEPRI, FAULTMASK, and
 * the NVIC determines the highest-priority pending exception.
 *
 * MProfileInterrupts is the CPU-side half of the interrupt path.
 * The device-side half lives in MProfileSCS (the NVIC).  Data flow:
 *
 *   SCS/NVIC state changes -> post()/clear() -> CPU checks at
 *   instruction boundary -> checkInterrupts() -> getInterrupt()
 *   returns ArmMFault -> CPU invokes fault -> updateIntrInfo()
 *   tells SCS to mark interrupt active.
 */

#include "arch/generic/interrupts.hh"
#include "params/MProfileInterrupts.hh"

namespace gem5
{

class MProfileSCS;  // forward -- defined in dev/arm/m_profile_scs.hh

namespace ArmISA
{

class MProfileInterrupts : public BaseInterrupts
{
  private:
    /** Pointer to the SCS/NVIC device.  Set during setThreadContext(). */
    MProfileSCS *scs = nullptr;

    /**
     * CPU-side pending flag.  Set by post() when the SCS determines
     * a deliverable interrupt exists; cleared by clear() or clearAll().
     * This is a coarse signal -- the actual priority check happens in
     * checkInterrupts() via scs->hasDeliverableIRQ().
     */
    bool pendingSignal = false;

    /**
     * Exception number of the last acknowledged interrupt.
     * Set by getInterrupt(), consumed by updateIntrInfo().
     */
    int lastAckedExcNum = -1;

  public:
    PARAMS(MProfileInterrupts);
    MProfileInterrupts(const Params &p);

    /**
     * Called during CPU init.  Discovers the SCS device via
     * dynamic_cast<ArmMSystem*> on the system pointer.
     */
    void setThreadContext(ThreadContext *tc) override;

    /** SCS signals that a deliverable interrupt may exist. */
    void post(int int_num, int index) override;

    /** SCS signals that the pending condition has cleared. */
    void clear(int int_num, int index) override;

    /** Reset: clear all pending signals. */
    void clearAll() override;

    /**
     * CPU calls this at instruction boundaries.
     * Returns true if PRIMASK allows it and SCS has a deliverable IRQ.
     */
    bool checkInterrupts() const override;

    /**
     * Returns the highest-priority pending interrupt as an ArmMFault.
     * Must only be called when checkInterrupts() returned true.
     */
    Fault getInterrupt() override;

    /**
     * Called after the CPU has taken the interrupt.
     * Tells the SCS to transition the interrupt from pending to active.
     */
    void updateIntrInfo() override;

    /**
     * Handle M-profile exception return (EXC_RETURN).
     * Deactivates the returning exception via SCS, then calls
     * mProfileExcReturnUnstack() to restore CPU state from the
     * exception frame.
     *
     * On real Cortex-M hardware, deactivation is automatic during
     * exception return (no software EOIR write like A-profile GIC).
     * This method models that hardware-automatic behavior.
     *
     * Called from BxMProfile::execute(), ExcReturnFromPC::execute(),
     * and MTLB::translateAtomic/Timing() when an EXC_RETURN address
     * is detected.
     */
    void excReturn(ThreadContext *tc, uint32_t exc_return);

    void serialize(CheckpointOut &cp) const override;
    void unserialize(CheckpointIn &cp) override;
};

} // namespace ArmISA
} // namespace gem5

#endif // __ARCH_ARM_M_INTERRUPTS_HH__
