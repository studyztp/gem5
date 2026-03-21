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
 * M-profile System Control Space (SCS) implementation.
 *
 * == Interrupt delivery data flow ==
 *
 * The SCS is the central hub for M-profile interrupt handling.  The
 * complete flow from peripheral assertion to CPU handler entry is:
 *
 *   1. Peripheral calls sendInt(irq)
 *        -> sets nvicPending[irq] bit
 *        -> calls updatePending()
 *
 *   2. updatePending() scans all interrupt sources:
 *        a. External IRQs: enabled AND pending AND NOT active
 *        b. System exceptions: SysTick (ICSR.PENDSTSET),
 *           PendSV (ICSR.PENDSVSET)
 *        -> finds the one with lowest priority number (= highest priority)
 *        -> compares against executionPriority() (current masking/ceiling)
 *        -> if deliverable: postToInterruptController() signals the CPU
 *        -> if not: clearFromInterruptController()
 *
 *   3. CPU (at instruction boundary) calls
 *      MProfileInterrupts::checkInterrupts()
 *        -> queries scs->hasDeliverableIRQ()
 *        -> returns true/false
 *
 *   4. CPU calls MProfileInterrupts::getInterrupt()
 *        -> calls scs->acknowledgeIRQ() to get the exception number
 *        -> returns ArmMFault(exc_num)
 *
 *   5. CPU invokes ArmMFault::invoke() (from m_faults.cc)
 *        -> pushes hardware exception frame to stack
 *        -> sets LR = EXC_RETURN
 *        -> reads handler address from VTOR + 4*exc_num
 *        -> branches to handler
 *
 *   6. CPU calls MProfileInterrupts::updateIntrInfo()
 *        -> calls scs->activateIRQ(exc_num)
 *        -> transitions interrupt from pending to active
 *        -> calls updatePending() again for next interrupt
 *
 * == SysTick flow ==
 *
 *   1. Firmware writes SysTick CSR with ENABLE=1
 *        -> sysTickSchedule() creates a gem5 event at curTick() + load*period
 *
 *   2. Event fires -> sysTickExpire()
 *        -> sets COUNTFLAG in CSR
 *        -> if TICKINT: sets ICSR.PENDSTSET -> updatePending()
 *        -> if still ENABLE: reloads and reschedules
 *
 *   3. Firmware reads CSR -> COUNTFLAG auto-clears (per spec)
 *
 * == Address map within 4KB SCS (offsets from 0xE000E000) ==
 *
 *   0x010-0x01F  SysTick: CSR(0x10), RVR(0x14), CVR(0x18), CALIB(0x1C)
 *   0x100-0x11F  NVIC ISER[0..7]   (set-enable)
 *   0x180-0x19F  NVIC ICER[0..7]   (clear-enable)
 *   0x200-0x21F  NVIC ISPR[0..7]   (set-pending)
 *   0x280-0x29F  NVIC ICPR[0..7]   (clear-pending)
 *   0x300-0x31F  NVIC IABR[0..7]   (active, read-only)
 *   0x400-0x4EF  NVIC IPR[0..59]   (priority, byte-accessible)
 *   0xD00-0xD3F  SCB registers     (bridged to ISA misc regs)
 *   0xF00        NVIC STIR          (software trigger)
 */

#include "dev/arm/m_profile_scs.hh"

#include <cassert>
#include <cstring>

#include "arch/arm/m_faults.hh"
#include "arch/arm/m_system.hh"
#include "arch/arm/regs/misc.hh"
#include "base/trace.hh"
#include "cpu/base.hh"
#include "cpu/thread_context.hh"
#include "debug/MProfileSCS.hh"
#include "mem/packet.hh"
#include "mem/packet_access.hh"
#include "sim/serialize.hh"

namespace gem5
{

// =========================================================================
// SCB register bridge
// =========================================================================
//
// Maps SCB register offsets (relative to 0xD00 within SCS) to ISA
// misc reg indices.  readSCB/writeSCB use this to forward MMIO
// accesses to tc->readMiscReg()/setMiscReg(), which routes through
// MISA and automatically applies all M-profile special handling:
//   - VTOR alignment mask (per vtor_align_bits)
//   - AIRCR VECTKEY check (reject writes with wrong key)
//   - W1C semantics for CFSR, HFSR, DFSR
//   - CPUID read-only enforcement
//   - xPSR T-bit sync from PCState

static const std::map<Addr, ArmISA::MiscRegIndex> scbRegMap = {
    {0x00, ArmISA::MISCREG_M_CPUID},   // CPUID (read-only)
    {0x04, ArmISA::MISCREG_M_ICSR},    // Interrupt Control and State
    {0x08, ArmISA::MISCREG_M_VTOR},    // Vector Table Offset
    {0x0C, ArmISA::MISCREG_M_AIRCR},   // Application Interrupt and Reset Ctrl
    {0x10, ArmISA::MISCREG_M_SCR},     // System Control
    {0x14, ArmISA::MISCREG_M_CCR},     // Configuration and Control
    {0x18, ArmISA::MISCREG_M_SHPR1},   // System Handler Priority 1
    {0x1C, ArmISA::MISCREG_M_SHPR2},   // System Handler Priority 2
    {0x20, ArmISA::MISCREG_M_SHPR3},   // System Handler Priority 3
    {0x24, ArmISA::MISCREG_M_SHCSR},   // System Handler Control and State
    {0x28, ArmISA::MISCREG_M_CFSR},    // Configurable Fault Status
    {0x2C, ArmISA::MISCREG_M_HFSR},    // HardFault Status
    {0x30, ArmISA::MISCREG_M_DFSR},    // Debug Fault Status
    {0x34, ArmISA::MISCREG_M_MMFAR},   // MemManage Fault Address
    {0x38, ArmISA::MISCREG_M_BFAR},    // BusFault Address
    {0x3C, ArmISA::MISCREG_M_AFSR},    // Auxiliary Fault Status
};

// =========================================================================
// SysTick helper struct constructor
// =========================================================================

MProfileSCS::SysTick::SysTick(MProfileSCS &parent)
    : ctrl(0), load(0), calib(0), startTick(0),
      expireEvent([&parent]{ parent.sysTickExpire(); },
                  "MProfileSCS::sysTickExpire")
{}

// =========================================================================
// Constructor
// =========================================================================

MProfileSCS::MProfileSCS(const Params &p)
    : BasicPioDevice(p, 0x1000),  // 4KB SCS region
      highestPendingExc(-1),
      highestPendingPri(0xFF),
      sysTick(*this),
      numIRQs(std::min(p.num_irqs, (uint32_t)MAX_IRQS)),
      priorityBits(p.priority_bits),
      priorityMask(0),
      hasSysTick(p.has_systick),
      hasBasepri(p.has_basepri)
{
    fatal_if(p.priority_bits < 2 || p.priority_bits > 8,
             "MProfileSCS: priority_bits=%u out of range [2,8].",
             (unsigned)p.priority_bits);
    fatal_if(p.num_irqs > MAX_IRQS,
             "MProfileSCS: num_irqs=%u exceeds maximum %d.",
             p.num_irqs, MAX_IRQS);

    // Priority mask: only the top N bits of the 8-bit priority field
    // are implemented.  Unimplemented low bits are always zero on
    // reads and ignored on writes.
    // Example: 4 bits -> shift (8-4)=4 -> mask 0xF0 (bits[7:4])
    priorityMask = (uint8_t)(~((1u << (8 - priorityBits)) - 1u));

    sysTick.calib = p.systick_calib;

    // Zero-init all NVIC state arrays
    std::memset(nvicEnabled,  0, sizeof(nvicEnabled));
    std::memset(nvicPending,  0, sizeof(nvicPending));
    std::memset(nvicActive,   0, sizeof(nvicActive));
    std::memset(nvicPriority, 0, sizeof(nvicPriority));

    // Register with ArmMSystem so MProfileInterrupts can discover us
    // via mSystem->getSCS() during BaseCPU::init().
    // Must happen in the constructor (not init()) because
    // MProfileInterrupts::setThreadContext() is called during
    // BaseCPU::init(), which may run before MProfileSCS::init().
    // This follows the A-profile BaseGic pattern (registers in ctor).
    mSystem = dynamic_cast<ArmMSystem *>(sys);
    assert(mSystem && "MProfileSCS must be attached to an ArmMSystem, "
                      "not a generic System.");
    mSystem->setSCS(this);
}

// =========================================================================
// init() -- called after all SimObjects are constructed
// =========================================================================

void
MProfileSCS::init()
{
    BasicPioDevice::init();
}

// =========================================================================
// startup() -- called after all SimObjects have been init()'d
// =========================================================================

void
MProfileSCS::startup()
{
    BasicPioDevice::startup();

    // Acquire ThreadContext for ISA misc reg access.
    // Done in startup() (not constructor/init()) because threads
    // are fully registered only after all init() calls complete
    // (BaseCPU::init() calls registerThreadContexts()).
    // Single-core MVP: always thread 0.
    tc = sys->threads[0];

    // After checkpoint restore, rebuild the cached pending state and
    // re-signal the CPU interrupt line.  This must happen here (not in
    // unserialize()) because updatePending() reads ISA misc regs via
    // tc, which is only available after startup() sets it above.
    if (restoredFromCheckpoint) {
        updatePending();
        restoredFromCheckpoint = false;
    }
}

// =========================================================================
// Top-level MMIO dispatch
// =========================================================================
//
// CPU loads/stores to the SCS region arrive here.  We decode the
// offset within the 4KB region and route to the appropriate handler.
//
// The address ranges are non-contiguous (per ARMv7-M architecture):
//   SysTick: 0x010-0x01F (4 registers, 16 bytes)
//   NVIC:    0x100-0x4EF (multiple register banks) + 0xF00 (STIR)
//   SCB:     0xD00-0xD3F (16 registers, 64 bytes)
//
// Gaps between ranges are reserved; reads return 0, writes ignored.

Tick
MProfileSCS::read(PacketPtr pkt)
{
    Addr daddr = pkt->getAddr() - pioAddr;
    unsigned size = pkt->getSize();

    // NVIC IPR registers (0x400-0x4EF) are byte-accessible per
    // DDI0403E B3.4.9.  CMSIS reads individual priority bytes via
    // LDRB at 0xE000E400+irq.  Handle sub-word IPR reads here;
    // 32-bit IPR reads fall through to readNVIC() below.
    if (daddr >= 0x400 && daddr <= 0x4EF && size < 4) {
        if (size == 1) {
            int irq = daddr - 0x400;
            uint8_t pri = (irq < (int)numIRQs)
                              ? nvicPriority[irq] : 0;
            pkt->setLE<uint8_t>(pri);
        } else if (size == 2) {
            uint16_t val = 0;
            for (int i = 0; i < 2; ++i) {
                int irq = (daddr - 0x400) + i;
                uint8_t pri = (irq < (int)numIRQs)
                                  ? nvicPriority[irq] : 0;
                val |= ((uint16_t)pri) << (i * 8);
            }
            pkt->setLE<uint16_t>(val);
        }
        pkt->makeAtomicResponse();
        return pioDelay;
    }

    uint32_t data = 0;

    if (daddr >= 0x010 && daddr <= 0x01F) {
        // SysTick registers (only if this variant has SysTick)
        if (hasSysTick)
            readSysTick(daddr - 0x010, data);
    } else if (daddr >= 0x100 && daddr <= 0x4EF) {
        // NVIC registers (ISER/ICER/ISPR/ICPR/IABR/IPR)
        // Range 0x100-0x4EF covers all NVIC banks including IABR at 0x300.
        readNVIC(daddr, data);
    } else if (daddr >= 0xD00 && daddr <= 0xD3F) {
        // SCB registers (bridged to ISA misc regs)
        readSCB(daddr - 0xD00, data);
    } else {
        warn("MProfileSCS: read from unimplemented offset %#x", daddr);
    }

    pkt->setLE<uint32_t>(data);
    pkt->makeAtomicResponse();
    return pioDelay;
}

Tick
MProfileSCS::write(PacketPtr pkt)
{
    Addr daddr = pkt->getAddr() - pioAddr;
    unsigned size = pkt->getSize();

    // NVIC IPR registers (0x400-0x4EF) are byte-accessible per
    // DDI0403E B3.4.9.  CMSIS writes individual priority bytes via
    // STRB at 0xE000E400+irq.  Handle sub-word IPR writes here;
    // 32-bit IPR writes fall through to writeNVIC() below.
    if (daddr >= 0x400 && daddr <= 0x4EF && size < 4) {
        if (size == 1) {
            int irq = daddr - 0x400;
            if (irq < (int)numIRQs) {
                nvicPriority[irq] =
                    pkt->getLE<uint8_t>() & priorityMask;
            }
        } else if (size == 2) {
            uint16_t val = pkt->getLE<uint16_t>();
            for (int i = 0; i < 2; ++i) {
                int irq = (daddr - 0x400) + i;
                if (irq < (int)numIRQs) {
                    nvicPriority[irq] =
                        (uint8_t)((val >> (i * 8)) & priorityMask);
                }
            }
        }
        updatePending();
        pkt->makeAtomicResponse();
        return pioDelay;
    }

    uint32_t data = pkt->getLE<uint32_t>();

    if (daddr >= 0x010 && daddr <= 0x01F) {
        // SysTick registers (only if this variant has SysTick)
        if (hasSysTick)
            writeSysTick(daddr - 0x010, data);
    } else if ((daddr >= 0x100 && daddr <= 0x4EF) ||
               (daddr == 0xF00)) {
        // NVIC registers (ISER/ICER/ISPR/ICPR/IPR/STIR)
        writeNVIC(daddr, data);
    } else if (daddr >= 0xD00 && daddr <= 0xD3F) {
        // SCB registers (bridged to ISA misc regs)
        writeSCB(daddr - 0xD00, data);
    } else {
        warn("MProfileSCS: write to unimplemented offset %#x", daddr);
    }

    pkt->makeAtomicResponse();
    return pioDelay;
}

// =========================================================================
// SCB bridge -- read/write forwarded to ISA misc regs
// =========================================================================
//
// The SCB contains configuration registers that firmware accesses via
// MMIO loads/stores (NOT via MRS/MSR -- those only access core special
// registers like MSP, PSP, CONTROL, PRIMASK, BASEPRI, FAULTMASK).
//
// We store SCB register values in ISA misc regs (MISCREG_M_*) and
// bridge MMIO accesses to tc->readMiscReg()/setMiscReg().  This
// reuses ALL the special handling already implemented in MISA:
//   - VTOR: alignment mask applied on write
//   - AIRCR: VECTKEY check on write (rejects wrong key)
//   - CFSR/HFSR/DFSR: W1C (write-1-to-clear) semantics
//   - CPUID: read-only (writes silently dropped)
//   - xPSR: T-bit synced from PCState on read

Tick
MProfileSCS::readSCB(Addr offset, uint32_t &data)
{
    auto it = scbRegMap.find(offset);
    if (it != scbRegMap.end()) {
        data = tc->readMiscReg(it->second);
    } else {
        warn("MProfileSCS: SCB read at unknown offset %#x", offset);
    }
    return pioDelay;
}

Tick
MProfileSCS::writeSCB(Addr offset, uint32_t data)
{
    using namespace ArmISA;

    // ICSR (offset 0x04) has special write semantics (DDI0403E B3.2.4):
    //   bit 28: PENDSVSET  — write 1 to pend PendSV
    //   bit 27: PENDSVCLR  — write 1 to clear PendSV pending
    //   bit 26: PENDSTSET  — write 1 to pend SysTick
    //   bit 25: PENDSTCLR  — write 1 to clear SysTick pending
    // These are NOT a normal read-modify-write register.  SET and CLR
    // are separate actions applied to the current ICSR value.
    //
    // After modifying pending bits, call updatePending() so the NVIC
    // re-evaluates whether an exception should be delivered to the CPU.
    // Without this, FreeRTOS context switches (PendSV) never fire.
    if (offset == 0x04) {
        RegVal icsr = tc->readMiscRegNoEffect(MISCREG_M_ICSR);

        // PENDSVSET / PENDSVCLR
        if (data & (1u << 28))
            icsr |= (1u << 28);    // pend PendSV
        if (data & (1u << 27))
            icsr &= ~(1u << 28);   // clear PendSV pending

        // PENDSTSET / PENDSTCLR
        if (data & (1u << 26))
            icsr |= (1u << 26);    // pend SysTick
        if (data & (1u << 25))
            icsr &= ~(1u << 26);   // clear SysTick pending

        tc->setMiscRegNoEffect(MISCREG_M_ICSR, icsr);
        updatePending();
        return pioDelay;
    }

    auto it = scbRegMap.find(offset);
    if (it != scbRegMap.end()) {
        tc->setMiscReg(it->second, data);
    } else {
        warn("MProfileSCS: SCB write at unknown offset %#x", offset);
    }
    return pioDelay;
}

// =========================================================================
// NVIC register handlers
// =========================================================================
//
// NVIC registers use special write semantics to avoid read-modify-write
// races (important for multi-master systems, though we're single-core):
//
//   ISER (Set-Enable):   write 1 to a bit -> enables that IRQ
//                         write 0 has no effect
//                         read returns current enabled state
//
//   ICER (Clear-Enable): write 1 to a bit -> disables that IRQ
//                         write 0 has no effect
//                         read returns current enabled state (same as ISER)
//
//   ISPR (Set-Pending):  write 1 to a bit -> pends that IRQ
//                         write 0 has no effect
//                         read returns current pending state
//
//   ICPR (Clear-Pending): write 1 to a bit -> un-pends that IRQ
//                          write 0 has no effect
//                          read returns current pending state (same as ISPR)
//
//   IABR (Active):       read-only; returns which IRQs are being serviced
//
//   IPR (Priority):      8-bit priority per IRQ, byte-accessible
//                         only top N bits implemented (N = priorityBits)
//                         unimplemented low bits always read 0
//
//   STIR (Software Trigger): write IRQ number to pend it
//
// All writes that change interrupt state call updatePending() to
// re-evaluate whether the CPU should be signalled.

Tick
MProfileSCS::readNVIC(Addr offset, uint32_t &data)
{
    if (offset >= 0x100 && offset <= 0x11F) {
        // ISER[0..7]: read returns enabled state
        int word = (offset - 0x100) / 4;
        data = nvicEnabled[word];

    } else if (offset >= 0x180 && offset <= 0x19F) {
        // ICER[0..7]: read also returns enabled state (not the clear mask)
        int word = (offset - 0x180) / 4;
        data = nvicEnabled[word];

    } else if (offset >= 0x200 && offset <= 0x21F) {
        // ISPR[0..7]: read returns pending state
        int word = (offset - 0x200) / 4;
        data = nvicPending[word];

    } else if (offset >= 0x280 && offset <= 0x29F) {
        // ICPR[0..7]: read also returns pending state
        int word = (offset - 0x280) / 4;
        data = nvicPending[word];

    } else if (offset >= 0x300 && offset <= 0x31F) {
        // IABR[0..7]: read-only active bits
        int word = (offset - 0x300) / 4;
        data = nvicActive[word];

    } else if (offset >= 0x400 && offset <= 0x4EF) {
        // IPR[0..59]: 4 priority bytes packed into each 32-bit word.
        // Firmware often reads a whole word and extracts bytes.
        int base_irq = ((offset - 0x400) / 4) * 4;
        data = 0;
        for (int i = 0; i < 4; ++i) {
            int irq = base_irq + i;
            uint8_t pri = (irq < (int)numIRQs) ? nvicPriority[irq] : 0;
            data |= ((uint32_t)pri) << (i * 8);
        }

    } else {
        warn("MProfileSCS: NVIC read at unknown offset %#x", offset);
    }
    return pioDelay;
}

Tick
MProfileSCS::writeNVIC(Addr offset, uint32_t data)
{
    if (offset >= 0x100 && offset <= 0x11F) {
        // ISER[0..7]: write-1-to-set (set enable bits)
        int word = (offset - 0x100) / 4;
        nvicEnabled[word] |= data;
        updatePending();

    } else if (offset >= 0x180 && offset <= 0x19F) {
        // ICER[0..7]: write-1-to-clear (clear enable bits)
        int word = (offset - 0x180) / 4;
        nvicEnabled[word] &= ~data;
        updatePending();

    } else if (offset >= 0x200 && offset <= 0x21F) {
        // ISPR[0..7]: write-1-to-set (set pending bits)
        int word = (offset - 0x200) / 4;
        nvicPending[word] |= data;
        updatePending();

    } else if (offset >= 0x280 && offset <= 0x29F) {
        // ICPR[0..7]: write-1-to-clear (clear pending bits)
        int word = (offset - 0x280) / 4;
        nvicPending[word] &= ~data;
        updatePending();

    } else if (offset >= 0x400 && offset <= 0x4EF) {
        // IPR[0..59]: 4 priority bytes packed per word.
        // Only the top priorityBits bits are implemented;
        // unimplemented low bits are ignored on write.
        int base_irq = ((offset - 0x400) / 4) * 4;
        for (int i = 0; i < 4; ++i) {
            int irq = base_irq + i;
            if (irq < (int)numIRQs) {
                nvicPriority[irq] =
                    (uint8_t)((data >> (i * 8)) & priorityMask);
            }
        }
        updatePending();

    } else if (offset == 0xF00) {
        // STIR (Software Trigger Interrupt Register):
        // Write the IRQ number (bits[8:0]) to pend that external IRQ.
        // This lets firmware trigger interrupts from software.
        uint32_t irq = data & 0x1FF;
        if (irq < numIRQs) {
            int word = irq / 32;
            nvicPending[word] |= (1u << (irq % 32));
            updatePending();
        }

    } else {
        warn("MProfileSCS: NVIC write at unknown offset %#x", offset);
    }
    return pioDelay;
}

// =========================================================================
// Priority evaluation
// =========================================================================
//
// executionPriority() returns the effective priority of the current
// execution context.  A pending interrupt is only deliverable if its
// priority is STRICTLY LESS than (= higher priority than) this value.
//
// The priority model (DDI0403E B1.5.4):
//   - Fixed priorities: Reset=-3, NMI=-2, HardFault=-1
//   - Configurable: everything else, 0..255 (only top N bits matter)
//   - FAULTMASK=1: boosts execution priority to -1 (blocks everything
//     except NMI).  We map this to unsigned 0 for comparison purposes
//     since no configurable exception has priority < 0.
//   - PRIMASK=1: boosts execution priority to 0 (blocks all
//     configurable-priority exceptions).
//   - BASEPRI!=0: sets a priority ceiling (exceptions with priority
//     >= BASEPRI are blocked).
//   - Active exceptions: the priority of the highest-priority active
//     exception becomes the execution priority.
//
// Lower number = higher priority.  0xFF = lowest (Thread mode, no masks).

uint8_t
MProfileSCS::executionPriority() const
{
    using namespace ArmISA;

    // FAULTMASK: if set, execution priority is -1 (blocks everything
    // except NMI).  Only available on M3/M4/M7 (not M0/M0+).
    // We represent -1 as 0 in unsigned, which is correct because
    // no configurable exception can have priority < 0.
    if (hasBasepri) {
        RegVal faultmask = tc->readMiscRegNoEffect(MISCREG_M_FAULTMASK);
        if (faultmask & 1)
            return 0;
    }

    // PRIMASK: if set, execution priority is 0 (blocks all
    // configurable-priority exceptions, i.e., everything except
    // NMI and HardFault).
    RegVal primask = tc->readMiscRegNoEffect(MISCREG_M_PRIMASK);
    if (primask & 1)
        return 0;

    // BASEPRI: if nonzero, acts as a priority ceiling.  Exceptions
    // with priority >= BASEPRI are blocked.  Only on M3/M4/M7.
    if (hasBasepri) {
        RegVal basepri = tc->readMiscRegNoEffect(MISCREG_M_BASEPRI);
        uint8_t bp = (uint8_t)(basepri & priorityMask);
        if (bp != 0)
            return bp;
    }

    // Scan active exception priorities to find the execution priority.
    // DDI0403E B1.5.4: the execution priority is the highest priority
    // (lowest number) among all active exceptions and priority masks.
    uint8_t execPri = 0xFF;  // default: Thread mode, no active exceptions

    // Check fixed-priority exceptions via xPSR.IPSR.
    // HardFault (exc 3): fixed priority -1, mapped to 0 in unsigned.
    // NMI (exc 2): fixed priority -2, also mapped to 0.
    // These don't have SHCSR active bits; IPSR is the only indicator.
    ArmISA::ArmMISA::XPSR xpsr = tc->readMiscRegNoEffect(MISCREG_M_XPSR);
    if (xpsr.exception == MPEXC_HARDFAULT ||
        xpsr.exception == MPEXC_NMI)
        return 0;  // nothing configurable can preempt

    // Check SHCSR active bits for system exceptions with configurable
    // priority.  DDI0403E B3.2.10 defines the active bit positions.
    // Group by SHPR register to minimize misc reg reads.
    RegVal shcsr = tc->readMiscRegNoEffect(MISCREG_M_SHCSR);

    // SHPR1 covers exceptions 4-7 (MemManage, BusFault, UsageFault)
    //   bit 0: MEMFAULTACT  (exc 4) → SHPR1[7:0]
    //   bit 1: BUSFAULTACT  (exc 5) → SHPR1[15:8]
    //   bit 3: USGFAULTACT  (exc 6) → SHPR1[23:16]
    if (shcsr & ((1u << 0) | (1u << 1) | (1u << 3))) {
        RegVal shpr1 = tc->readMiscRegNoEffect(MISCREG_M_SHPR1);
        if (shcsr & (1u << 0)) {  // MEMFAULTACT
            uint8_t pri = (uint8_t)(shpr1 & priorityMask);
            if (pri < execPri) execPri = pri;
        }
        if (shcsr & (1u << 1)) {  // BUSFAULTACT
            uint8_t pri = (uint8_t)((shpr1 >> 8) & priorityMask);
            if (pri < execPri) execPri = pri;
        }
        if (shcsr & (1u << 3)) {  // USGFAULTACT
            uint8_t pri = (uint8_t)((shpr1 >> 16) & priorityMask);
            if (pri < execPri) execPri = pri;
        }
    }

    // SHPR2 covers exceptions 8-11
    //   bit 7: SVCALLACT (exc 11) → SHPR2[31:24]
    if (shcsr & (1u << 7)) {
        RegVal shpr2 = tc->readMiscRegNoEffect(MISCREG_M_SHPR2);
        uint8_t pri = (uint8_t)((shpr2 >> 24) & priorityMask);
        if (pri < execPri) execPri = pri;
    }

    // SHPR3 covers exceptions 12-15
    //   bit 8:  MONITORACT  (exc 12) → SHPR3[7:0]
    //   bit 10: PENDSVACT   (exc 14) → SHPR3[23:16]
    //   bit 11: SYSTICKACT  (exc 15) → SHPR3[31:24]
    if (shcsr & ((1u << 8) | (1u << 10) | (1u << 11))) {
        RegVal shpr3 = tc->readMiscRegNoEffect(MISCREG_M_SHPR3);
        if (shcsr & (1u << 8)) {  // MONITORACT
            uint8_t pri = (uint8_t)(shpr3 & priorityMask);
            if (pri < execPri) execPri = pri;
        }
        if (shcsr & (1u << 10)) {  // PENDSVACT
            uint8_t pri = (uint8_t)((shpr3 >> 16) & priorityMask);
            if (pri < execPri) execPri = pri;
        }
        if (shcsr & (1u << 11)) {  // SYSTICKACT
            uint8_t pri = (uint8_t)((shpr3 >> 24) & priorityMask);
            if (pri < execPri) execPri = pri;
        }
    }

    // Scan NVIC active bits for external IRQs (exc >= 16).
    // Each bit in nvicActive[] corresponds to one IRQ.
    for (int w = 0; w < MAX_IRQS / 32; ++w) {
        uint32_t active = nvicActive[w];
        while (active) {
            int bit = __builtin_ctz(active);  // lowest set bit
            int irq = w * 32 + bit;
            if (irq < (int)numIRQs) {
                uint8_t pri = nvicPriority[irq] & priorityMask;
                if (pri < execPri) execPri = pri;
            }
            active &= ~(1u << bit);
        }
    }

    return execPri;
}

// =========================================================================
// updatePending() -- central interrupt evaluation
// =========================================================================
//
// Called after any change to interrupt state (enable, pending, active,
// priority, or mask registers).  Scans all interrupt sources to find
// the highest-priority deliverable one, then signals or de-signals
// the CPU accordingly.
//
// "Deliverable" means:
//   1. The interrupt is enabled AND pending AND NOT already active
//   2. Its priority is strictly less than executionPriority()
//
// This function maintains the cached (highestPendingExc, highestPendingPri)
// so that hasDeliverableIRQ() and acknowledgeIRQ() are O(1).

void
MProfileSCS::updatePending()
{
    using namespace ArmISA;

    highestPendingExc = -1;
    highestPendingPri = 0xFF;

    // --- Scan external IRQs ---
    // For each 32-bit word of IRQs, compute the set that is
    // enabled AND pending AND not already active, then find
    // the one with lowest priority number in that set.
    for (int word = 0; word < (int)((numIRQs + 31) / 32); ++word) {
        uint32_t deliverable = nvicEnabled[word] & nvicPending[word]
                               & ~nvicActive[word];
        while (deliverable) {
            int bit = __builtin_ctz(deliverable);
            int irq = word * 32 + bit;
            uint8_t pri = nvicPriority[irq] & priorityMask;
            if (pri < highestPendingPri) {
                highestPendingPri = pri;
                highestPendingExc = MPEXC_EXTERNAL_BASE + irq;
            }
            deliverable &= deliverable - 1;  // clear lowest set bit
        }
    }

    // --- Check system exceptions pended via ICSR ---
    // SysTick (exc 15) and PendSV (exc 14) are pended by setting
    // bits in ICSR, not through the NVIC enable/pending arrays.
    // Their priorities come from SHPR3 (System Handler Priority 3):
    //   SHPR3[31:24] = SysTick priority
    //   SHPR3[23:16] = PendSV priority
    RegVal icsr = tc->readMiscRegNoEffect(MISCREG_M_ICSR);

    // SysTick: ICSR bit 26 = PENDSTSET
    if (hasSysTick && (icsr & (1u << 26))) {
        RegVal shpr3 = tc->readMiscRegNoEffect(MISCREG_M_SHPR3);
        uint8_t pri = (uint8_t)((shpr3 >> 24) & priorityMask);
        if (pri < highestPendingPri) {
            highestPendingPri = pri;
            highestPendingExc = MPEXC_SYSTICK;
        }
    }

    // PendSV: ICSR bit 28 = PENDSVSET
    if (icsr & (1u << 28)) {
        RegVal shpr3 = tc->readMiscRegNoEffect(MISCREG_M_SHPR3);
        uint8_t pri = (uint8_t)((shpr3 >> 16) & priorityMask);
        if (pri < highestPendingPri) {
            highestPendingPri = pri;
            highestPendingExc = MPEXC_PENDSV;
        }
    }

    // --- Signal or de-signal the CPU ---
    // Only post if the pending interrupt can actually preempt
    // the current execution (priority comparison).
    if (highestPendingExc >= 0 &&
        highestPendingPri < executionPriority()) {
        postToInterruptController();
    } else {
        clearFromInterruptController();
    }
}

// =========================================================================
// NVIC external interface -- called by peripheral devices
// =========================================================================

void
MProfileSCS::sendInt(uint32_t irq)
{
    if (irq >= numIRQs) {
        warn("MProfileSCS::sendInt: IRQ %u out of range (max %u)",
             irq, numIRQs - 1);
        return;
    }
    // Set the pending bit for this IRQ.  The IRQ must also be enabled
    // (in ISER) for it to be considered deliverable by updatePending().
    int word = irq / 32;
    nvicPending[word] |= (1u << (irq % 32));
    updatePending();
}

void
MProfileSCS::clearInt(uint32_t irq)
{
    if (irq >= numIRQs)
        return;
    // Clear the pending bit.  If the IRQ was the highest-priority
    // pending, updatePending() will find the next one (or none).
    int word = irq / 32;
    nvicPending[word] &= ~(1u << (irq % 32));
    updatePending();
}

// =========================================================================
// CPU-side interface -- called by MProfileInterrupts
// =========================================================================

bool
MProfileSCS::hasDeliverableIRQ() const
{
    // Quick check using cached state from updatePending().
    if (highestPendingExc < 0)
        return false;
    return highestPendingPri < executionPriority();
}

int
MProfileSCS::acknowledgeIRQ()
{
    // Return the exception number that updatePending() determined.
    // The caller (MProfileInterrupts::getInterrupt) wraps this in
    // an ArmMFault and returns it to the CPU pipeline.
    return highestPendingExc;
}

void
MProfileSCS::activateIRQ(int exc_num)
{
    using namespace ArmISA;

    // Transition the interrupt from pending to active.
    // For external IRQs: clear pending bit, set active bit.
    // For system exceptions: clear the ICSR pending bit.
    // This is called by MProfileInterrupts::updateIntrInfo() after
    // the CPU has entered the exception handler.

    if (exc_num >= MPEXC_EXTERNAL_BASE) {
        int irq = exc_num - MPEXC_EXTERNAL_BASE;
        int word = irq / 32;
        int bit = irq % 32;
        nvicActive[word] |= (1u << bit);
        nvicPending[word] &= ~(1u << bit);

    } else if (exc_num == MPEXC_SYSTICK) {
        // Clear ICSR.PENDSTSET (bit 26) and set SHCSR.SYSTICKACT (bit 11)
        // to track that SysTick handler is now active.
        RegVal icsr = tc->readMiscRegNoEffect(MISCREG_M_ICSR);
        icsr &= ~(1u << 26);
        tc->setMiscRegNoEffect(MISCREG_M_ICSR, icsr);
        RegVal shcsr = tc->readMiscRegNoEffect(MISCREG_M_SHCSR);
        shcsr |= (1u << 11);  // SYSTICKACT
        tc->setMiscRegNoEffect(MISCREG_M_SHCSR, shcsr);

    } else if (exc_num == MPEXC_PENDSV) {
        // Clear ICSR.PENDSVSET (bit 28) and set SHCSR.PENDSVACT (bit 10)
        // to track that PendSV handler is now active.
        RegVal icsr = tc->readMiscRegNoEffect(MISCREG_M_ICSR);
        icsr &= ~(1u << 28);
        tc->setMiscRegNoEffect(MISCREG_M_ICSR, icsr);
        RegVal shcsr = tc->readMiscRegNoEffect(MISCREG_M_SHCSR);
        shcsr |= (1u << 10);  // PENDSVACT
        tc->setMiscRegNoEffect(MISCREG_M_SHCSR, shcsr);
    }

    // Re-evaluate: there may be another pending interrupt that can
    // preempt even the newly-active handler (nested interrupts).
    updatePending();
}

void
MProfileSCS::deactivateIRQ(int exc_num)
{
    using namespace ArmISA;

    // Called on exception return (EXC_RETURN).  Transitions the
    // interrupt from active to inactive, allowing it to pend again.

    if (exc_num >= MPEXC_EXTERNAL_BASE) {
        // External IRQ: clear nvicActive bit.
        int irq = exc_num - MPEXC_EXTERNAL_BASE;
        int word = irq / 32;
        int bit = irq % 32;
        nvicActive[word] &= ~(1u << bit);

    } else {
        // System exception: clear the corresponding SHCSR active bit.
        // Uses the shared helper from m_faults.hh which maps exception
        // numbers to SHCSR bit positions (DDI0403E B3.2.10).
        int shcsrBit = mProfileShcsrActiveBit(exc_num);
        if (shcsrBit >= 0) {
            RegVal shcsr = tc->readMiscRegNoEffect(MISCREG_M_SHCSR);
            shcsr &= ~(1u << shcsrBit);
            tc->setMiscRegNoEffect(MISCREG_M_SHCSR, shcsr);
        }
    }

    // Re-evaluate: a lower-priority interrupt that was blocked by
    // the now-deactivated handler may become deliverable.
    updatePending();
}

// =========================================================================
// CPU interrupt signalling helpers
// =========================================================================

void
MProfileSCS::postToInterruptController()
{
    // Signal the CPU that an interrupt is ready for delivery.
    // The CPU's checkInterrupts() will then call back into
    // hasDeliverableIRQ() and getInterrupt().
    // First arg is ThreadID — must match tc, not hardcoded 0.
    if (tc)
        tc->getCpuPtr()->postInterrupt(tc->threadId(), 0, 0);
}

void
MProfileSCS::clearFromInterruptController()
{
    // Remove the interrupt signal from the CPU.
    // This happens when the pending interrupt is no longer deliverable
    // (e.g., masked by PRIMASK, or cleared by firmware).
    if (tc)
        tc->getCpuPtr()->clearInterrupt(tc->threadId(), 0, 0);
}

// =========================================================================
// SysTick timer
// =========================================================================
//
// SysTick is a 24-bit countdown timer.  When enabled (CSR.ENABLE=1),
// it counts down from the LOAD value at the device clock rate.
// When it reaches 0:
//   - COUNTFLAG (CSR bit 16) is set
//   - If TICKINT (CSR bit 1) is set, SysTick exception (exc 15) is pended
//   - Timer reloads from LOAD and continues counting
//
// Register semantics (DDI0403E B3.3):
//   CSR (offset 0x00):
//     [0]  ENABLE    - enable/disable the counter
//     [1]  TICKINT   - enable interrupt on reaching 0
//     [2]  CLKSOURCE - 1=processor clock, 0=external ref (we always use proc)
//     [16] COUNTFLAG - set when counter reaches 0; clears on CSR read
//     Writes: only bits[2:0] are writable; bit 16 is read-only
//
//   RVR (offset 0x04):
//     [23:0] RELOAD - value loaded on timer restart; must be nonzero
//     Writes: masked to 24 bits
//
//   CVR (offset 0x08):
//     [23:0] CURRENT - current counter value
//     Read: returns computed value from scheduled event time
//     Write: writing ANY value clears current value, clears COUNTFLAG,
//            and restarts the timer if ENABLE is set
//
//   CALIB (offset 0x0C):
//     Read-only; returns implementation-defined calibration value

Tick
MProfileSCS::readSysTick(Addr offset, uint32_t &data)
{
    switch (offset) {
      case 0x00:  // CSR
        data = sysTick.ctrl;
        // COUNTFLAG auto-clears on read (DDI0403E B3.3.1).
        // This is how firmware polls for timer expiry without interrupts.
        sysTick.ctrl &= ~(1u << 16);
        break;
      case 0x04:  // RVR (reload value)
        data = sysTick.load;
        break;
      case 0x08:  // CVR (current value, computed from event schedule)
        data = sysTickCurrentValue();
        break;
      case 0x0C:  // CALIB (read-only, from Python param)
        data = sysTick.calib;
        break;
      default:
        warn("MProfileSCS: SysTick read at unknown offset %#x", offset);
        break;
    }
    return pioDelay;
}

Tick
MProfileSCS::writeSysTick(Addr offset, uint32_t data)
{
    switch (offset) {
      case 0x00: {  // CSR
        bool was_enabled = sysTick.ctrl & 1;

        // Only bits[2:0] are writable.  Preserve COUNTFLAG (bit 16).
        sysTick.ctrl = (sysTick.ctrl & (1u << 16)) | (data & 0x7);

        bool now_enabled = sysTick.ctrl & 1;

        // Start or stop the countdown event as needed
        if (now_enabled && !was_enabled) {
            sysTickSchedule();
        } else if (!now_enabled && was_enabled) {
            if (sysTick.expireEvent.scheduled())
                deschedule(sysTick.expireEvent);
        }
        break;
      }
      case 0x04:  // RVR: 24-bit reload value
        sysTick.load = data & 0x00FFFFFF;
        break;
      case 0x08:  // CVR: writing any value clears counter + COUNTFLAG
        sysTick.ctrl &= ~(1u << 16);
        if (sysTick.expireEvent.scheduled())
            deschedule(sysTick.expireEvent);
        // Restart countdown from LOAD if timer is enabled
        if (sysTick.ctrl & 1)
            sysTickSchedule();
        break;
      case 0x0C:  // CALIB: read-only, writes ignored
        break;
      default:
        warn("MProfileSCS: SysTick write at unknown offset %#x", offset);
        break;
    }
    return pioDelay;
}

void
MProfileSCS::sysTickExpire()
{
    using namespace ArmISA;

    // Counter has reached 0.  Set COUNTFLAG so firmware can see it.
    sysTick.ctrl |= (1u << 16);

    // If TICKINT is enabled, pend the SysTick exception (exc 15)
    // by setting ICSR.PENDSTSET (bit 26).  updatePending() will
    // then determine if it's deliverable (based on priority).
    if (sysTick.ctrl & (1u << 1)) {
        RegVal icsr = tc->readMiscRegNoEffect(MISCREG_M_ICSR);
        icsr |= (1u << 26);  // PENDSTSET
        tc->setMiscRegNoEffect(MISCREG_M_ICSR, icsr);
        updatePending();
    }

    // Reload and reschedule if timer is still enabled.
    // The counter wraps from 0 back to LOAD on each expiry.
    if (sysTick.ctrl & 1)
        sysTickSchedule();
}

void
MProfileSCS::sysTickSchedule()
{
    // Don't schedule if LOAD is 0 (counter would never expire)
    if (sysTick.load == 0)
        return;

    // Schedule expiry at curTick() + ((LOAD + 1) * clockPeriod).
    // The counter counts from LOAD down to 0 inclusive (LOAD+1 cycles).
    // Per DDI0403E B3.3: "the timer counts down from RELOAD to zero".
    Tick delay = clockPeriod() * ((Tick)sysTick.load + 1);
    sysTick.startTick = curTick();

    if (sysTick.expireEvent.scheduled())
        deschedule(sysTick.expireEvent);
    schedule(sysTick.expireEvent, curTick() + delay);
}

uint32_t
MProfileSCS::sysTickCurrentValue() const
{
    // Current value is computed from how much time remains until
    // the scheduled expiry event.  If the timer isn't running,
    // the current value is 0 (per spec: CVR reads 0 when disabled).
    if (!sysTick.expireEvent.scheduled())
        return 0;

    // Guard against underflow: if curTick has advanced past the
    // event time (possible due to scheduling granularity), return 0.
    if (curTick() >= sysTick.expireEvent.when())
        return 0;

    Tick remaining = sysTick.expireEvent.when() - curTick();
    return (uint32_t)(remaining / clockPeriod());
}

// =========================================================================
// Checkpoint serialization
//
// Saves all NVIC and SysTick runtime state.  SCB registers (SHCSR,
// SHPR1-3, ICSR, VTOR, etc.) live in ISA misc regs and are serialized
// by MISA::serialize() — they are NOT duplicated here.
//
// Configuration params (numIRQs, priorityBits, hasSysTick, etc.) are
// re-initialized by the constructor from Python and need not be saved.
//
// Pattern follows GicV2 (NVIC arrays) and Sp804::Timer (event schedule).
// =========================================================================

void
MProfileSCS::serialize(CheckpointOut &cp) const
{
    // NVIC interrupt state arrays (bit-vectors and per-IRQ priorities)
    SERIALIZE_ARRAY(nvicEnabled, MAX_IRQS / 32);
    SERIALIZE_ARRAY(nvicPending, MAX_IRQS / 32);
    SERIALIZE_ARRAY(nvicActive,  MAX_IRQS / 32);
    SERIALIZE_ARRAY(nvicPriority, MAX_IRQS);

    // Cached highest-pending state (rebuilt by updatePending() on
    // unserialize, but saved for completeness / debugging)
    SERIALIZE_SCALAR(highestPendingExc);
    SERIALIZE_SCALAR(highestPendingPri);

    // SysTick timer state
    SERIALIZE_SCALAR(sysTick.ctrl);
    SERIALIZE_SCALAR(sysTick.load);
    SERIALIZE_SCALAR(sysTick.startTick);

    // SysTick expiry event schedule (Sp804 timer pattern):
    // Save whether the event is scheduled and, if so, at what tick.
    bool sysTickEventScheduled = sysTick.expireEvent.scheduled();
    SERIALIZE_SCALAR(sysTickEventScheduled);
    if (sysTickEventScheduled) {
        Tick sysTickEventTime = sysTick.expireEvent.when();
        SERIALIZE_SCALAR(sysTickEventTime);
    }
}

void
MProfileSCS::unserialize(CheckpointIn &cp)
{
    // NVIC interrupt state arrays
    UNSERIALIZE_ARRAY(nvicEnabled, MAX_IRQS / 32);
    UNSERIALIZE_ARRAY(nvicPending, MAX_IRQS / 32);
    UNSERIALIZE_ARRAY(nvicActive,  MAX_IRQS / 32);
    UNSERIALIZE_ARRAY(nvicPriority, MAX_IRQS);

    // Cached highest-pending state
    UNSERIALIZE_SCALAR(highestPendingExc);
    UNSERIALIZE_SCALAR(highestPendingPri);

    // SysTick timer state
    UNSERIALIZE_SCALAR(sysTick.ctrl);
    UNSERIALIZE_SCALAR(sysTick.load);
    UNSERIALIZE_SCALAR(sysTick.startTick);

    // Restore SysTick expiry event if it was scheduled at checkpoint time
    bool sysTickEventScheduled;
    UNSERIALIZE_SCALAR(sysTickEventScheduled);
    if (sysTickEventScheduled) {
        Tick sysTickEventTime;
        UNSERIALIZE_SCALAR(sysTickEventTime);
        schedule(sysTick.expireEvent, sysTickEventTime);
    }

    // Do NOT call updatePending() here — it accesses tc (ThreadContext)
    // which is not set until startup().  gem5 restore order is:
    //   constructor → init() → unserialize() → startup()
    // Instead, set a flag so startup() calls updatePending() after
    // tc is available.
    restoredFromCheckpoint = true;
}

} // namespace gem5
