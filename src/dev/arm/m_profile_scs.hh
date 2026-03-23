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

#include <functional>
#include <queue>
#include <vector>

#include "arch/arm/m_faults.hh"
#include "arch/arm/m_system.hh"
#include "arch/arm/regs/misc.hh"
#include "arch/arm/regs/misc_types.hh"
#include "cpu/thread_context.hh"
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
    static constexpr uint32_t MAX_NUM_IRQS = 496;
    static constexpr uint8_t MAX_PRIR_BITS = 8;
    // TODO: capping at 1 for now.
    static constexpr uint8_t MAX_NUM_SYSTICKS = 1;

    struct SysTick
    {
      uint32_t ctrl;   // CSR: ENABLE[0], TICKINT[1], CLKSOURCE[2],
                       //      COUNTFLAG[16]
      uint32_t load;   // RVR: 24-bit reload value
      uint32_t calib;  // CALIB: read-only calibration
      Tick startTick;  // curTick() when last (re)started
      EventFunctionWrapper expireEvent;

      SysTick(MProfileSCS &parent, uint8_t index);
    };

    /**
     * Represents any M-profile exception with configurable priority.
     *
     * Used for both external IRQs (exc 16+) and system exceptions
     * (exc 4-15: MemManage, BusFault, UsageFault, SVCall, DebugMonitor,
     * PendSV, SysTick).  External IRQs additionally use the 'enabled'
     * field; system exceptions manage enable state via SHCSR bits.
     *
     * interruptNum is the exception number (not 0-based IRQ number):
     *   - System exceptions: 4-15
     *   - External IRQs: 16+
     */
    struct Interrupt
    {
      bool active;
      bool enabled;
      bool pending;
      int16_t priority;
      uint32_t interruptNum;

      // Track whether this interrupt is currently in the pending
      // or active priority queues, to avoid duplicate insertions.
      bool inPendingQueue;
      bool inActiveQueue;

      Interrupt(bool active, bool enabled, bool pending, int16_t priority,
                uint32_t interruptNum, bool inPendingQueue, bool inActiveQueue)
          : active(active), enabled(enabled), pending(pending),
            priority(priority), interruptNum(interruptNum),
            inPendingQueue(inPendingQueue), inActiveQueue(inActiveQueue)
      {}

      bool operator<(const Interrupt& other) const {
          return priority < other.priority;
      }
    };

    struct InterruptPtrCompare
    {
      // min-heap: lowest priority number = highest priority = top.
      // Tie-break by exception number: lower exception number wins
      // (DDI0403E B1.5.4).
      bool operator()(const Interrupt* a, const Interrupt* b) const {
        if (a->priority == b->priority) {
          return a->interruptNum > b->interruptNum;
        }
        return a->priority > b->priority;
      }
    };

  private:
    uint32_t numIrqs;
    uint8_t priorityBits;
    uint8_t irqpriorityMask;
    uint8_t numSysticks;
    bool hasBasePri;

    // -- Exception masking state --
    // Cached from MSR writes to mask registers.  Updated by
    // setupMask() which is called from m_insts.cc whenever
    // firmware writes PRIMASK, BASEPRI, BASEPRI_MAX, or FAULTMASK.
    // This avoids reading misc regs on every priority check.

    /** PRIMASK: when true, raises execution priority to 0,
     *  blocking all configurable-priority exceptions.
     *  Only NMI and HardFault can fire. */
    bool primask = false;

    /** FAULTMASK: when true, raises execution priority to -1,
     *  blocking everything except NMI.  Auto-cleared on
     *  exception return (except from NMI). */
    bool faultmask = false;

    /** BASEPRI threshold: exceptions with priority >= this value
     *  are blocked.  0 means no masking (disabled). */
    int16_t basepri = 0;

    /** Lowest priority number among all active exceptions.
     *  An exception can only preempt if its priority is strictly
     *  less than this value.  256 = no active exceptions.
     *  Updated by activateIRQ(), deactivateIRQ(), and SHCSR writes. */
    int16_t activePriorityCeiling = 256;

    std::vector<SysTick> sysTicks;

    // All configurable-priority exceptions: system exceptions (exc 4-15)
    // and external IRQs (exc 16+).  Indexed by exception number.
    std::vector<Interrupt> interrupts;

    std::priority_queue<Interrupt*, std::vector<Interrupt*>,
                        InterruptPtrCompare> pendingInterrupts;
    std::priority_queue<Interrupt*, std::vector<Interrupt*>,
                        InterruptPtrCompare> activeInterrupts;

    // -- SCB register storage --
    // Stored directly in SCS instead of misc regs.  Firmware
    // accesses these via MMIO loads/stores to 0xE000ED00-0xE000ED3F.

    uint32_t cpuid;          // 0xD00: read-only, from platform
    // VTOR (0xD08) is stored in MISA misc reg (MISCREG_M_VTOR),
    // not here.  MISA owns the alignment mask (vtor_align_bits).
    // SCS reads/writes VTOR through tc->readMiscRegNoEffect() and
    // tc->setMiscReg() which applies the alignment mask.
    // AIRCR (0xD0C) is stored in MISA misc reg (MISCREG_M_AIRCR).
    // MISA handles the VECTKEY check on write.  SCS reads/writes
    // through tc.  Contains PRIGROUP (not yet implemented) and
    // SYSRESETREQ (not yet implemented).
    //
    // SCR (0xD10) is stored in MISA misc reg (MISCREG_M_SCR).
    // Controls low-power behavior (DDI0403E B3.2.7):
    //   bit[1] SLEEPONEXIT — enter sleep on return from last ISR
    //   bit[2] SLEEPDEEP   — WFI/WFE uses deep sleep
    //   bit[4] SEVONPEND   — pending interrupt wakes from sleep
    // TODO: Not modeled.  Firmware can read/write SCR but the bits
    // have no effect.  Modeling requires a CPU sleep/suspend model.
    //
    // CCR (0xD14) is stored in MISA misc reg (MISCREG_M_CCR).
    // m_faults.cc reads CCR.STKALIGN from the misc reg during
    // exception entry.  SCS reads/writes through tc.
    uint32_t cfsr = 0;       // 0xD28: configurable fault status (W1C)
    uint32_t hfsr = 0;       // 0xD2C: hardfault status (W1C)
    uint32_t dfsr = 0;       // 0xD30: debug fault status (W1C)
    uint32_t mmfar = 0;      // 0xD34: memmanage fault address
    uint32_t bfar = 0;       // 0xD38: busfault address

    ThreadContext *tc = nullptr;
    ArmMSystem *mSystem = nullptr;

  public:
    PARAMS(MProfileSCS);
    MProfileSCS(const Params &p);

    // -- Exception mask interface (called by m_insts.cc on MSR) --

    /**
     * Update cached exception mask state.
     *
     * Called by MsrMProfile::execute() when firmware writes to
     * PRIMASK, BASEPRI, BASEPRI_MAX, or FAULTMASK via MSR.
     * Caches the mask values so SCS doesn't need to read misc
     * regs on every priority check.
     *
     * @param reg   The misc register being written (e.g.,
     *              MISCREG_M_PRIMASK, MISCREG_M_BASEPRI, etc.).
     * @param value The value being written to the register.
     */
    void setupMask(ArmISA::MiscRegIndex reg, int16_t value);

    // -- Exception Checking
    bool updatePending();

    // -- MMIO interface (called by CPU memory system) --

    Tick read(PacketPtr pkt) override;
    Tick write(PacketPtr pkt) override;

    // -- Peripheral device interface (called by hardware models) --

    void sendInt(uint32_t irq);
    void clearInt(uint32_t irq);

    // -- CPU-side interface (called by MProfileInterrupts) --

    bool hasDeliverableIRQ();
    int acknowledgeIRQ();
    bool activateIRQ(int exc_num);
    void deactivateIRQ(int exc_num);

    // -- gem5 lifecycle --

    void init() override;
    void startup() override;
    void serialize(CheckpointOut &cp) const override;
    void unserialize(CheckpointIn &cp) override;

    // -- SysTick timer --

    /** SysTick expired: set COUNTFLAG, pend exc 15 if TICKINT,
     *  reload and reschedule if still enabled. */
    void sysTickExpire(uint8_t index);

    /** Schedule the SysTick expiry event based on LOAD value.
     *  Fires after (LOAD+1) clock cycles. */
    void sysTickSchedule(uint8_t index);

    /** Compute current SysTick counter value from scheduled event. */
    uint32_t sysTickCurrentValue(uint8_t index) const;

  protected:
    /**
     * Initialize system exception entries (interrupts[0..15]).
     *
     * Sets up the first 16 entries in the interrupts vector:
     *   0     Reserved
     *   1     Reset       — fixed priority -3
     *   2     NMI         — fixed priority -2
     *   3     HardFault   — fixed priority -1
     *   4     MemManage   — enabled via SHCSR.MEMFAULTENA
     *   5     BusFault    — enabled via SHCSR.BUSFAULTENA
     *   6     UsageFault  — enabled via SHCSR.USGFAULTENA
     *   7-10  Reserved
     *   11    SVCall      — always enabled
     *   12    DebugMonitor — always enabled
     *   13    Reserved
     *   14    PendSV      — always enabled
     *   15    SysTick     — always enabled
     *
     * All start with active=false, pending=false.
     */
    void initSysInterrupts();

    /**
     * Reset all interrupt state to architectural reset values.
     *
     * Resets system exceptions (0-15) via initSysInterrupts(),
     * clears all external IRQ entries (16+) to disabled/inactive/
     * not-pending/priority-0, and drains the pending and active
     * priority queues.
     *
     * Called from: constructor, MProfileReset::invoke(), and
     * checkpoint restore.
     */
    void resetAllInterrupts();

    /**
     * Check if an interrupt can be activated given current masks
     * and the active exception priority.
     *
     * Returns true if no mask blocks it and its priority is
     * strictly higher (lower number) than the current active
     * exception.  Does not modify any state.
     */
    bool canActivate(const Interrupt &intr) const;

    /**
     * Set an interrupt to pending and add it to the pending queue
     * if not already there.
     */
    void pendInterrupt(Interrupt &intr);

    // -- Internal read/write helpers --

    /** Read a 32-bit register value by aligned SCS offset.
     *  Used for sub-word read-modify-write. */
    uint32_t readRegByAddr(Addr alignedAddr);

    /** Reconstruct a 32-bit NVIC bitmap word from interrupts[].
     *  field: 'e'=enabled, 'p'=pending, 'a'=active. */
    uint32_t readNvicBits(Addr addr, Addr base, char field);

    /** Read an IPR word (4 packed priority bytes). */
    uint32_t readIpr(Addr addr);

    /** Read an SCB register by offset from 0xD00. */
    uint32_t readScb(Addr offset);

    /** Pack 4 exception priorities into a SHPR word. */
    uint32_t readShpr(uint32_t baseExc);

    /** Compute ICSR value on-the-fly from SCS state. */
    uint32_t computeICSR();

    /** Compute SHCSR value on-the-fly from interrupts[]. */
    uint32_t computeSHCSR();

    /** Merge sub-word write data into an existing 32-bit value. */
    uint32_t mergeSubWord(uint32_t existing, PacketPtr pkt,
                          unsigned size, int byteOffset);

    /** Apply W1S action to NVIC bitmap bits. */
    void writeNvicW1S(Addr addr, Addr base, uint32_t data,
                      std::function<void(Interrupt&)> action);

    /** Apply W1C action to NVIC bitmap bits. */
    void writeNvicW1C(Addr addr, Addr base, uint32_t data,
                      std::function<void(Interrupt&)> action);

    /** Write an SCB register by offset from 0xD00. */
    void writeScb(Addr offset, uint32_t data);
};

} // namespace gem5

#endif // __DEV_ARM_M_PROFILE_SCS_HH__
