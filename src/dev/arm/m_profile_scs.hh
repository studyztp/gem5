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

#ifndef __DEV_ARM_M_PROFILE_SCS_HH__
#define __DEV_ARM_M_PROFILE_SCS_HH__

/** @file
 * M-profile System Control Space (SCS) device model.
 *
 * Models the 4KB region at 0xE000E000 containing the SCB, NVIC, and
 * SysTick as a single BasicPioDevice with internal address dispatch.
 * SCB registers are bridged to ISA misc regs via ThreadContext.
 */

#include "dev/io_device.hh"
#include "params/MProfileSCS.hh"
#include "sim/eventq.hh"

namespace gem5
{

class ArmMSystem;
class ThreadContext;

class MProfileSCS : public BasicPioDevice
{
  public:
    PARAMS(MProfileSCS);
    MProfileSCS(const Params &p);

    /** Handle MMIO read from CPU. */
    Tick read(PacketPtr pkt) override;

    /** Handle MMIO write from CPU. */
    Tick write(PacketPtr pkt) override;

    // -- NVIC external interface (called by peripheral devices) --

    /** Assert an external IRQ line (0-based, 0..numIRQs-1). */
    void sendInt(uint32_t irq);

    /** De-assert an external IRQ line. */
    void clearInt(uint32_t irq);

    // -- CPU-side interface (called by MProfileInterrupts) --

    /** True if a pending interrupt can preempt current execution. */
    bool hasDeliverableIRQ() const;

    /** Return exception number of highest-priority pending IRQ. */
    int acknowledgeIRQ();

    /** Transition interrupt from pending to active after entry. */
    void activateIRQ(int exc_num);

    /** Transition interrupt from active to inactive on return. */
    void deactivateIRQ(int exc_num);

    /** Re-scan all sources and update the CPU interrupt signal. */
    void updatePending();

    /** Register with ArmMSystem; acquire ThreadContext. */
    void init() override;

  protected:
    // -- Address dispatch handlers --
    // Offset is relative to the sub-module's register block.

    Tick readSysTick(Addr offset, uint32_t &data);
    Tick writeSysTick(Addr offset, uint32_t data);
    Tick readNVIC(Addr offset, uint32_t &data);
    Tick writeNVIC(Addr offset, uint32_t data);
    Tick readSCB(Addr offset, uint32_t &data);
    Tick writeSCB(Addr offset, uint32_t data);

    // -- NVIC state (external IRQs 0..numIRQs-1) --
    // Bit-vector arrays following the GicV2 pattern.  Sized for the
    // architectural maximum; only words covering 0..numIRQs-1 matter.

    static constexpr int MAX_IRQS = 240;
    uint32_t nvicEnabled[MAX_IRQS / 32];   // ISER/ICER
    uint32_t nvicPending[MAX_IRQS / 32];   // ISPR/ICPR
    uint32_t nvicActive[MAX_IRQS / 32];    // IABR (read-only)
    uint8_t  nvicPriority[MAX_IRQS];       // IPR (8-bit per IRQ)

    /** Cached highest-priority pending exception (-1 = none). */
    int highestPendingExc;

    /** Priority of highestPendingExc (0xFF = none pending). */
    uint8_t highestPendingPri;

    // -- SysTick state --
    // 24-bit countdown timer.  When enabled, a gem5 event fires
    // after (load * clockPeriod) ticks; on expiry COUNTFLAG is set
    // and (if TICKINT) exception 15 is pended.

    struct SysTick
    {
        uint32_t ctrl;   // CSR: ENABLE[0], TICKINT[1], CLKSOURCE[2],
                         //      COUNTFLAG[16]
        uint32_t load;   // RVR: 24-bit reload value
        uint32_t calib;  // CALIB: read-only calibration
        Tick startTick;  // curTick() when last (re)started

        EventFunctionWrapper expireEvent;

        SysTick(MProfileSCS &parent);
    } sysTick;

    // -- Configuration (from Python params) --
    // See MProfileSCS.py for per-knob rationale.

    uint32_t numIRQs;      // External IRQ count (max 240)
    uint8_t priorityBits;  // Implemented priority bits (2-8)
    uint8_t priorityMask;  // Mask of implemented high bits
                           // e.g. 4 bits -> 0xF0
    bool hasSysTick;       // SysTick present? (some M0: no)
    bool hasBasepri;       // BASEPRI/FAULTMASK? (M0: no)

    // -- System references (set during init()) --

    ThreadContext *tc = nullptr;
    ArmMSystem *mSystem = nullptr;

    // -- Internal helpers --

    /** Signal CPU that a deliverable interrupt exists. */
    void postToInterruptController();

    /** Clear the CPU interrupt signal. */
    void clearFromInterruptController();

    /** SysTick expired: set COUNTFLAG, optionally pend exc 15. */
    void sysTickExpire();

    /** Schedule SysTick expiry based on current load value. */
    void sysTickSchedule();

    /** Compute current SysTick counter from scheduled event time. */
    uint32_t sysTickCurrentValue() const;

    /**
     * Compute current execution priority (lower = higher priority).
     *
     * Checks FAULTMASK (if hasBasepri), PRIMASK, BASEPRI (if
     * hasBasepri), and active exception priorities.  Thread mode
     * with no masks returns 0xFF (lowest priority).
     */
    uint8_t executionPriority() const;
};

} // namespace gem5

#endif // __DEV_ARM_M_PROFILE_SCS_HH__
