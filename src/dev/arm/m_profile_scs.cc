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

#include "dev/arm/m_profile_scs.hh"

#include "cpu/base.hh"
#include "mem/packet_access.hh"

namespace gem5
{

using namespace ArmISA;
using namespace ArmMISA;

// -- Constructor --

MProfileSCS::SysTick::SysTick(MProfileSCS &parent, uint8_t index)
    : ctrl(0), load(0), calib(0), startTick(0),
        expireEvent([&parent, index]{ parent.sysTickExpire(index); },
            "MProfileSCS::sysTickExpire" + std::to_string(index))
{}

void
MProfileSCS::initSysInterrupts()
{
    // Set system exception entries (interrupts[0..15]) to their
    // architectural reset state.  All entries must already exist
    // in the vector (created with defaults by the constructor).
    //
    // Reusable for: constructor setup, reset, checkpoint restore.

    // Exc 0: Reserved.
    interrupts[0] = {false, false, false, 0, 0, false, false};

    //                    active enabled pending pri  excNum inPQ  inAQ
    // Exc 1: Reset — fixed priority -3, always enabled.
    interrupts[1]  = {false, true,  false, -3, 1,  false, false};

    // Exc 2: NMI — fixed priority -2, always enabled.
    interrupts[2]  = {false, true,  false, -2, 2,  false, false};

    // Exc 3: HardFault — fixed priority -1, always enabled.
    interrupts[3]  = {false, true,  false, -1, 3,  false, false};

    // Exc 4: MemManage — configurable, disabled by default (SHCSR).
    interrupts[4]  = {false, false, false, 0,  4,  false, false};

    // Exc 5: BusFault — configurable, disabled by default (SHCSR).
    interrupts[5]  = {false, false, false, 0,  5,  false, false};

    // Exc 6: UsageFault — configurable, disabled by default (SHCSR).
    interrupts[6]  = {false, false, false, 0,  6,  false, false};

    // Exc 7-10: Reserved.
    interrupts[7]  = {false, false, false, 0,  7,  false, false};
    interrupts[8]  = {false, false, false, 0,  8,  false, false};
    interrupts[9]  = {false, false, false, 0,  9,  false, false};
    interrupts[10] = {false, false, false, 0,  10, false, false};

    // Exc 11: SVCall — configurable priority, always enabled.
    interrupts[11] = {false, true,  false, 0,  11, false, false};

    // Exc 12: DebugMonitor — configurable priority, always enabled.
    interrupts[12] = {false, true,  false, 0,  12, false, false};

    // Exc 13: Reserved.
    interrupts[13] = {false, false, false, 0,  13, false, false};

    // Exc 14: PendSV — configurable priority, always enabled.
    interrupts[14] = {false, true,  false, 0,  14, false, false};

    // Exc 15: SysTick — configurable priority, always enabled.
    interrupts[15] = {false, true,  false, 0,  15, false, false};
}

void
MProfileSCS::resetAllInterrupts()
{
    // Reset system exceptions (exc 0-15) to architectural reset state.
    initSysInterrupts();

    // Reset all external IRQs (exc 16+) to disabled, inactive,
    // not pending, priority 0.  Per DDI0403E Table B3-8, all NVIC
    // registers (ISER, ISPR, IABR, IPR) reset to 0x00000000.
    for (uint32_t i = 16; i < interrupts.size(); ++i) {
        interrupts[i] = {false, false, false, 0, i, false, false};
    }

    // Drain the priority queues — no exceptions pending or active
    // after reset.
    pendingInterrupts = {};
    activeInterrupts = {};

    // Reset mask and priority ceiling state.
    primask = false;
    faultmask = false;
    basepri = 0;
    activePriorityCeiling = 256;

    // Reset SCB registers to architectural reset values.
    // DDI0403E Table B3-4.
    // Note: cpuid is not reset here — it's a fixed platform value
    // set in startup().
    // VTOR is in MISA misc reg — reset via tc->clearArchRegs() or
    // MProfileReset::invoke(), not here.
    // AIRCR, SCR, CCR are in MISA misc regs — reset via
    // clearArchRegs().  Not reset here.
    // CCR is in MISA misc reg — reset via clearArchRegs().
    // Not reset here.
    cfsr = 0;
    hfsr = 0;
    dfsr = 0;
    mmfar = 0;
    bfar = 0;

    // Reset SysTick state.
    for (uint8_t i = 0; i < sysTicks.size(); ++i) {
        sysTicks[i].ctrl = 0;
        sysTicks[i].load = 0;
        sysTicks[i].startTick = 0;
        if (sysTicks[i].expireEvent.scheduled())
            deschedule(sysTicks[i].expireEvent);
    }
}

MProfileSCS::MProfileSCS(const Params &p)
    : BasicPioDevice(p, 0x1000),  // 4KB SCS region
    numIrqs(std::min(p.num_irqs, MAX_NUM_IRQS)),
    priorityBits(std::min(p.priority_bits, MAX_PRIR_BITS)),
    irqpriorityMask(0),
    numSysticks(std::min(p.num_systick, MAX_NUM_SYSTICKS)),
    hasBasePri(p.has_basepri)
{
    // Priority mask: only the top N bits of the 8-bit priority field
    // are implemented.  Unimplemented low bits are always zero on
    // reads and ignored on writes.
    // Example: 4 bits -> shift (8-4)=4 -> mask 0xF0 (bits[7:4])
    irqpriorityMask = (uint8_t)(~((1u << (8 - priorityBits)) - 1u));

    // Create all exception entries: system (0-15) + external IRQs (16+).
    // All start with default state (inactive, disabled, not pending,
    // priority 0).  initSysInterrupts() then sets the correct state
    // for the system exceptions.
    interrupts.resize(16 + numIrqs,
                      {false, false, false, 0, 0, false, false});

    resetAllInterrupts();

    // Setup systicks
    sysTicks.reserve(numSysticks);
    for (uint8_t i = 0; i < numSysticks; ++i) {
        sysTicks.emplace_back(*this, i);
        sysTicks[i].calib = p.systick_calib;
    }

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

// -- Exception mask interface --

void
MProfileSCS::setupMask(ArmISA::MiscRegIndex reg, int16_t value)
{
    using namespace ArmISA;

    switch (reg) {
      case MISCREG_M_PRIMASK:
        // PRIMASK bit[0]: 1 = raise execution priority to 0,
        // blocking all configurable-priority exceptions.
        primask = (value & 1);
        break;

      case MISCREG_M_FAULTMASK:
        // FAULTMASK bit[0]: 1 = raise execution priority to -1,
        // blocking everything except NMI.
        // Only available on ARMv7-M+ (hasBasePri).
        if (hasBasePri)
            faultmask = (value & 1);
        break;

      case MISCREG_M_BASEPRI:
        // BASEPRI[7:0]: when non-zero, blocks exceptions with
        // priority >= this value.  Only the implemented high bits
        // matter.  0 = no masking.
        // Only available on ARMv7-M+ (hasBasePri).
        if (hasBasePri)
            basepri = (int16_t)(value & irqpriorityMask);
        break;

      case MISCREG_M_BASEPRI_MAX:
        // BASEPRI_MAX: conditional write — only updates BASEPRI
        // if the new value is non-zero AND has a lower priority
        // number (higher priority) than the current BASEPRI.
        // DDI0403E B5.2.3.
        if (hasBasePri) {
            int16_t masked = (int16_t)(value & irqpriorityMask);
            if (masked != 0 && (basepri == 0 || masked < basepri))
                basepri = masked;
        }
        break;

      default:
        break;
    }
}

// -- gem5 lifecycle --

void
MProfileSCS::init()
{
    BasicPioDevice::init();
    // TODO
}

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

    // CPUID from platform — not needed until firmware reads
    // 0xE000ED00, but safe to cache here.
    cpuid = mSystem->getCPUID();
}

// -- MMIO interface --

Tick
MProfileSCS::read(PacketPtr pkt)
{
    Addr daddr = pkt->getAddr() - pioAddr;
    unsigned size = pkt->getSize();
    Addr alignedAddr = daddr & ~0x3;

    // Read the full 32-bit register value at the aligned address.
    uint32_t data = readRegByAddr(alignedAddr);

    // SysTick CSR special case: COUNTFLAG (bit 16) auto-clears on
    // read (DDI0403E B3.3.1).  This is how firmware polls for timer
    // expiry without using interrupts.
    if (alignedAddr >= 0x010 && alignedAddr <= 0x01F) {
        Addr stOff = alignedAddr - 0x010;
        if (stOff == 0x00) {
            uint8_t stIndex = 0;
            sysTicks[stIndex].ctrl &= ~(1u << 16);
        }
    }

    // Extract the requested byte(s) from the 32-bit value.
    switch (size) {
      case 1: {
        int byteOffset = daddr & 0x3;
        pkt->setLE<uint8_t>((data >> (byteOffset * 8)) & 0xFF);
        break;
      }
      case 2: {
        int byteOffset = daddr & 0x2;
        pkt->setLE<uint16_t>((data >> (byteOffset * 8)) & 0xFFFF);
        break;
      }
      default:
        pkt->setLE<uint32_t>(data);
        break;
    }

    pkt->makeAtomicResponse();
    return pioDelay;
}

// Helper: read a 32-bit register value by aligned SCS offset.
// Used for sub-word read-modify-write on non-W1C registers.
uint32_t
MProfileSCS::readRegByAddr(Addr alignedAddr)
{
    if (alignedAddr >= 0x010 && alignedAddr <= 0x01F) {
        uint8_t stIndex = 0;
        Addr stOff = alignedAddr - 0x010;
        switch (stOff) {
          case 0x00: return sysTicks[stIndex].ctrl;
          case 0x04: return sysTicks[stIndex].load;
          case 0x08: return sysTickCurrentValue(stIndex);
          case 0x0C: return sysTicks[stIndex].calib;
          default:   return 0;
        }
    } else if (alignedAddr >= 0x100 && alignedAddr <= 0x11F) {
        // ISER: read returns enabled bitmap
        return readNvicBits(alignedAddr, 0x100, /*field=*/'e');
    } else if (alignedAddr >= 0x180 && alignedAddr <= 0x19F) {
        // ICER: read returns same enabled bitmap as ISER (DDI0403E B3.4.5)
        return readNvicBits(alignedAddr, 0x180, /*field=*/'e');
    } else if (alignedAddr >= 0x200 && alignedAddr <= 0x21F) {
        // ISPR: read returns pending bitmap
        return readNvicBits(alignedAddr, 0x200, /*field=*/'p');
    } else if (alignedAddr >= 0x280 && alignedAddr <= 0x29F) {
        // ICPR: read returns same pending bitmap as ISPR (DDI0403E B3.4.7)
        return readNvicBits(alignedAddr, 0x280, /*field=*/'p');
    } else if (alignedAddr >= 0x300 && alignedAddr <= 0x31F) {
        // IABR: read returns active bitmap (read-only)
        return readNvicBits(alignedAddr, 0x300, /*field=*/'a');
    } else if (alignedAddr >= 0x400 && alignedAddr <= 0x4EF) {
        return readIpr(alignedAddr);
    } else if (alignedAddr >= 0xD00 && alignedAddr <= 0xD3F) {
        return readScb(alignedAddr - 0xD00);
    }
    return 0;
}

// Helper: reconstruct a 32-bit NVIC bitmap word from interrupts[].
// field: 'e'=enabled, 'p'=pending, 'a'=active
uint32_t
MProfileSCS::readNvicBits(Addr addr, Addr base, char field)
{
    int word = (addr - base) / 4;
    uint32_t val = 0;
    for (int bit = 0; bit < 32; ++bit) {
        uint32_t excNum = word * 32 + bit + 16;
        if (excNum >= interrupts.size()) break;
        bool flag = false;
        switch (field) {
          case 'e': flag = interrupts[excNum].enabled; break;
          case 'p': flag = interrupts[excNum].pending; break;
          case 'a': flag = interrupts[excNum].active;  break;
        }
        if (flag) val |= (1u << bit);
    }
    return val;
}

// Helper: read IPR (4 packed priority bytes).
uint32_t
MProfileSCS::readIpr(Addr addr)
{
    uint32_t baseIrq = ((addr - 0x400) / 4) * 4;
    uint32_t val = 0;
    for (int i = 0; i < 4; ++i) {
        uint32_t excNum = baseIrq + i + 16;
        if (excNum < interrupts.size())
            val |= ((uint32_t)(interrupts[excNum].priority & 0xFF)
                    << (i * 8));
    }
    return val;
}

// Helper: read an SCB register by offset from 0xD00.
uint32_t
MProfileSCS::readScb(Addr offset)
{
    switch (offset) {
      case 0x00: return cpuid;
      case 0x04: return computeICSR();
      case 0x08: return tc->readMiscRegNoEffect(ArmISA::MISCREG_M_VTOR);
      case 0x0C: return tc->readMiscRegNoEffect(ArmISA::MISCREG_M_AIRCR);
      case 0x10:
        warn_once("MProfileSCS: SCR read — sleep behavior not modeled");
        return tc->readMiscRegNoEffect(ArmISA::MISCREG_M_SCR);
      case 0x14: return tc->readMiscRegNoEffect(ArmISA::MISCREG_M_CCR);
      case 0x18: return readShpr(4);   // SHPR1: exc 4-7
      case 0x1C: return readShpr(8);   // SHPR2: exc 8-11
      case 0x20: return readShpr(12);  // SHPR3: exc 12-15
      case 0x24: return computeSHCSR();
      case 0x28: return cfsr;
      case 0x2C: return hfsr;
      case 0x30: return dfsr;
      case 0x34: return mmfar;
      case 0x38: return bfar;
      default:
        warn("MProfileSCS: SCB read at unknown offset %#x", offset);
        return 0;
    }
}

// Helper: pack 4 exception priorities into a SHPR word.
uint32_t
MProfileSCS::readShpr(uint32_t baseExc)
{
    uint32_t val = 0;
    for (int i = 0; i < 4; ++i) {
        uint32_t excNum = baseExc + i;
        if (excNum < interrupts.size())
            val |= ((uint32_t)(interrupts[excNum].priority
                    & irqpriorityMask) << (i * 8));
    }
    return val;
}

// Helper: compute ICSR value on-the-fly from SCS state.
uint32_t
MProfileSCS::computeICSR()
{
    uint32_t icsr = 0;

    // VECTACTIVE [8:0]: current exception number from xPSR.IPSR.
    ArmMISA::XPSR xpsr = tc->readMiscRegNoEffect(
        ArmISA::MISCREG_M_XPSR);
    icsr |= ((uint32_t)xpsr.exception & 0x1FF);

    // VECTPENDING [20:12]: highest-priority pending exception.
    if (!pendingInterrupts.empty())
        icsr |= ((pendingInterrupts.top()->interruptNum & 0x1FF) << 12);

    // ISRPENDING [22]: 1 if any external IRQ is pending.
    for (uint32_t i = 16; i < interrupts.size(); ++i) {
        if (interrupts[i].pending && interrupts[i].enabled) {
            icsr |= (1u << 22);
            break;
        }
    }

    // RETTOBASE [11]: 1 if only one or zero exceptions active.
    int activeCount = 0;
    for (uint32_t i = 0; i < interrupts.size() && activeCount < 2; ++i) {
        if (interrupts[i].active) ++activeCount;
    }
    if (activeCount <= 1)
        icsr |= (1u << 11);

    // PENDSVSET [28] / PENDSTSET [26]
    if (interrupts[14].pending) icsr |= (1u << 28);
    if (interrupts[15].pending) icsr |= (1u << 26);

    return icsr;
}

// Helper: compute SHCSR value on-the-fly from interrupts[].
uint32_t
MProfileSCS::computeSHCSR()
{
    uint32_t val = 0;
    // Active bits
    if (interrupts[4].active)   val |= (1u << 0);   // MEMFAULTACT
    if (interrupts[5].active)   val |= (1u << 1);   // BUSFAULTACT
    if (interrupts[6].active)   val |= (1u << 3);   // USGFAULTACT
    if (interrupts[11].active)  val |= (1u << 7);   // SVCALLACT
    if (interrupts[12].active)  val |= (1u << 8);   // MONITORACT
    if (interrupts[14].active)  val |= (1u << 10);  // PENDSVACT
    if (interrupts[15].active)  val |= (1u << 11);  // SYSTICKACT
    // Pending bits
    if (interrupts[6].pending)  val |= (1u << 12);  // USGFAULTPENDED
    if (interrupts[4].pending)  val |= (1u << 13);  // MEMFAULTPENDED
    if (interrupts[5].pending)  val |= (1u << 14);  // BUSFAULTPENDED
    if (interrupts[11].pending) val |= (1u << 15);  // SVCALLPENDED
    // Enable bits
    if (interrupts[4].enabled)  val |= (1u << 16);  // MEMFAULTENA
    if (interrupts[5].enabled)  val |= (1u << 17);  // BUSFAULTENA
    if (interrupts[6].enabled)  val |= (1u << 18);  // USGFAULTENA
    return val;
}

// Helper: merge sub-word write data into existing register value.
uint32_t
MProfileSCS::mergeSubWord(uint32_t existing, PacketPtr pkt,
                          unsigned size, int byteOffset)
{
    if (size == 1) {
        uint8_t byte = pkt->getLE<uint8_t>();
        existing &= ~(0xFFu << (byteOffset * 8));
        existing |= ((uint32_t)byte << (byteOffset * 8));
    } else {
        uint16_t hw = pkt->getLE<uint16_t>();
        existing &= ~(0xFFFFu << (byteOffset * 8));
        existing |= ((uint32_t)hw << (byteOffset * 8));
    }
    return existing;
}

Tick
MProfileSCS::write(PacketPtr pkt)
{
    Addr daddr = pkt->getAddr() - pioAddr;
    unsigned size = pkt->getSize();
    Addr alignedAddr = daddr & ~0x3;
    uint32_t data;

    // Sub-word write handling.
    if (size < 4) {
        int byteOffset = daddr & 0x3;

        // W1C registers: zero-extend sub-word data so that only
        // the written byte(s) clear bits (DDI0403E B3.2.15).
        bool isW1C = (alignedAddr == 0xD28 || alignedAddr == 0xD2C ||
                      alignedAddr == 0xD30);
        if (isW1C) {
            data = 0;
            if (size == 1)
                data = (uint32_t)pkt->getLE<uint8_t>() << (byteOffset * 8);
            else
                data = (uint32_t)pkt->getLE<uint16_t>() << (byteOffset * 8);
        } else {
            uint32_t existing = readRegByAddr(alignedAddr);
            data = mergeSubWord(existing, pkt, size, byteOffset);
        }
        daddr = alignedAddr;
    } else {
        data = pkt->getLE<uint32_t>();
    }

    // ---- Address dispatch ----

    if (daddr >= 0x010 && daddr <= 0x01F) {
        // -- SysTick (offset 0x10-0x1F) --
        uint8_t stIndex = 0;
        SysTick &st = sysTicks[stIndex];
        Addr stOff = daddr - 0x010;

        switch (stOff) {
          case 0x00: {  // CSR
            bool wasEnabled = st.ctrl & 1;
            st.ctrl = (st.ctrl & (1u << 16)) | (data & 0x7);
            bool nowEnabled = st.ctrl & 1;
            if (nowEnabled && !wasEnabled)
                sysTickSchedule(stIndex);
            else if (!nowEnabled && wasEnabled && st.expireEvent.scheduled())
                deschedule(st.expireEvent);
            break;
          }
          case 0x04:  // RVR
            st.load = data & 0x00FFFFFF;
            break;
          case 0x08:  // CVR: write clears counter + COUNTFLAG
            st.ctrl &= ~(1u << 16);
            if (st.expireEvent.scheduled())
                deschedule(st.expireEvent);
            if (st.ctrl & 1)
                sysTickSchedule(stIndex);
            break;
          case 0x0C:  // CALIB: read-only
            break;
          default:
            warn("MProfileSCS: SysTick write at unknown offset %#x", daddr);
        }

    } else if (daddr >= 0x100 && daddr <= 0x11F) {
        // -- ISER: write-1-to-set enable --
        writeNvicW1S(daddr, 0x100, data,
            [](Interrupt &i) { i.enabled = true; });

    } else if (daddr >= 0x180 && daddr <= 0x19F) {
        // -- ICER: write-1-to-clear enable --
        writeNvicW1C(daddr, 0x180, data,
            [](Interrupt &i) { i.enabled = false; });

    } else if (daddr >= 0x200 && daddr <= 0x21F) {
        // -- ISPR: write-1-to-set pending --
        writeNvicW1S(daddr, 0x200, data,
            [this](Interrupt &i) { pendInterrupt(i); });

    } else if (daddr >= 0x280 && daddr <= 0x29F) {
        // -- ICPR: write-1-to-clear pending --
        writeNvicW1C(daddr, 0x280, data,
            [](Interrupt &i) { i.pending = false; });

    } else if (daddr >= 0x300 && daddr <= 0x31F) {
        // -- IABR: read-only --

    } else if (daddr >= 0x400 && daddr <= 0x4EF) {
        // -- IPR: 4 packed priority bytes per word --
        uint32_t baseIrq = ((daddr - 0x400) / 4) * 4;
        for (int i = 0; i < 4; ++i) {
            uint32_t excNum = baseIrq + i + 16;
            if (excNum < interrupts.size())
                interrupts[excNum].priority =
                    (int16_t)((data >> (i * 8)) & irqpriorityMask);
        }

    } else if (daddr >= 0xD00 && daddr <= 0xD3F) {
        // -- SCB registers --
        writeScb(daddr - 0xD00, data);

    } else if (daddr == 0xF00) {
        // -- STIR: software trigger --
        uint32_t excNum = (data & 0x1FF) + 16;
        if (excNum < interrupts.size())
            pendInterrupt(interrupts[excNum]);

    } else {
        warn("MProfileSCS: write to unimplemented offset %#x", daddr);
    }

    pkt->makeAtomicResponse();
    return pioDelay;
}

// Helper: apply a W1S (write-1-to-set) action to NVIC bitmap bits.
void
MProfileSCS::writeNvicW1S(Addr addr, Addr base, uint32_t data,
                           std::function<void(Interrupt&)> action)
{
    int word = (addr - base) / 4;
    uint32_t bits = data;
    while (bits) {
        int bit = __builtin_ctz(bits);  // find lowest set bit
        uint32_t excNum = word * 32 + bit + 16;
        if (excNum < interrupts.size())
            action(interrupts[excNum]);
        bits &= bits - 1;  // clear lowest set bit
    }
}

// Helper: apply a W1C (write-1-to-clear) action to NVIC bitmap bits.
void
MProfileSCS::writeNvicW1C(Addr addr, Addr base, uint32_t data,
                           std::function<void(Interrupt&)> action)
{
    int word = (addr - base) / 4;
    uint32_t bits = data;
    while (bits) {
        int bit = __builtin_ctz(bits);
        uint32_t excNum = word * 32 + bit + 16;
        if (excNum < interrupts.size())
            action(interrupts[excNum]);
        bits &= bits - 1;
    }
}

// SCB write dispatch.
void
MProfileSCS::writeScb(Addr offset, uint32_t data)
{
    switch (offset) {
      case 0x00:  // CPUID: read-only
        break;

      case 0x04:  // ICSR
        if (data & (1u << 28))  pendInterrupt(interrupts[14]);  // PENDSVSET
        if (data & (1u << 27))  interrupts[14].pending = false; // PENDSVCLR
        if (data & (1u << 26))  pendInterrupt(interrupts[15]);  // PENDSTSET
        if (data & (1u << 25))  interrupts[15].pending = false; // PENDSTCLR
        // TODO: bit 31 NMIPENDSET
        break;

      case 0x08:  // VTOR
        // VTOR write goes through MISA which applies the alignment
        // mask (vtor_align_bits).  Use setMiscReg (not NoEffect) so
        // the mask is applied.
        tc->setMiscReg(ArmISA::MISCREG_M_VTOR, data);
        break;

      case 0x0C:  // AIRCR
        // MISA handles VECTKEY check and VECTKEYSTAT readback via
        // setMiscReg().  Pass through — MISA rejects wrong key.
        tc->setMiscReg(ArmISA::MISCREG_M_AIRCR, data);
        // TODO: SYSRESETREQ (bit 2)
        break;

      case 0x10:  // SCR
        warn_once("MProfileSCS: SCR write — sleep behavior not modeled");
        tc->setMiscRegNoEffect(ArmISA::MISCREG_M_SCR, data);
        break;
      case 0x14:  // CCR
        tc->setMiscRegNoEffect(ArmISA::MISCREG_M_CCR, data);
        break;

      case 0x18:  // SHPR1: exc 4-7
      case 0x1C:  // SHPR2: exc 8-11
      case 0x20:  // SHPR3: exc 12-15
      {
        uint32_t baseExc = 4 + ((offset - 0x18) / 4) * 4;
        for (int i = 0; i < 4; ++i) {
            uint32_t excNum = baseExc + i;
            if (excNum < 16)
                interrupts[excNum].priority =
                    (int16_t)((data >> (i * 8)) & irqpriorityMask);
        }
        break;
      }

      case 0x24: {  // SHCSR
        // Enable bits
        interrupts[4].enabled  = (data >> 16) & 1;  // MEMFAULTENA
        interrupts[5].enabled  = (data >> 17) & 1;  // BUSFAULTENA
        interrupts[6].enabled  = (data >> 18) & 1;  // USGFAULTENA
        // Pending bits
        if ((data >> 12) & 1) pendInterrupt(interrupts[6]);   // USGFAULTPENDED
        else interrupts[6].pending = false;
        if ((data >> 13) & 1) pendInterrupt(interrupts[4]);   // MEMFAULTPENDED
        else interrupts[4].pending = false;
        if ((data >> 14) & 1) pendInterrupt(interrupts[5]);   // BUSFAULTPENDED
        else interrupts[5].pending = false;
        if ((data >> 15) & 1) pendInterrupt(interrupts[11]);  // SVCALLPENDED
        else interrupts[11].pending = false;
        // Active bits (dangerous — for context switch save/restore).
        // These don't cause exception entry/return, just affect
        // the priority ceiling via activePriorityCeiling.
        interrupts[4].active   = (data >> 0) & 1;   // MEMFAULTACT
        interrupts[5].active   = (data >> 1) & 1;   // BUSFAULTACT
        interrupts[6].active   = (data >> 3) & 1;   // USGFAULTACT
        interrupts[11].active  = (data >> 7) & 1;   // SVCALLACT
        interrupts[12].active  = (data >> 8) & 1;   // MONITORACT
        interrupts[14].active  = (data >> 10) & 1;  // PENDSVACT
        interrupts[15].active  = (data >> 11) & 1;  // SYSTICKACT

        // Recompute activePriorityCeiling from the SHCSR-managed
        // system exceptions.  Only these exceptions can have their
        // active state set/cleared via SHCSR writes.
        {
            static const int shcsrExcs[] = {4, 5, 6, 11, 12, 14, 15};
            activePriorityCeiling = 256;
            for (int exc : shcsrExcs) {
                if (interrupts[exc].active &&
                    interrupts[exc].priority < activePriorityCeiling)
                    activePriorityCeiling = interrupts[exc].priority;
            }
        }
        break;
      }

      case 0x28:  cfsr &= ~data; break;   // CFSR (W1C)
      case 0x2C:  hfsr &= ~data; break;   // HFSR (W1C)
      case 0x30:  dfsr &= ~data; break;   // DFSR (W1C)
      case 0x34:  mmfar = data;  break;   // MMFAR
      case 0x38:  bfar = data;   break;   // BFAR

      default:
        warn("MProfileSCS: SCB write at unknown offset %#x", offset);
    }
}

// -- Peripheral device interface --

void
MProfileSCS::sendInt(uint32_t irq)
{
    // TODO
}

void
MProfileSCS::clearInt(uint32_t irq)
{
    // TODO
}

// -- CPU-side interface --

bool
MProfileSCS::hasDeliverableIRQ()
{
    // updatePending() returns true only if it promoted a NEW pending
    // exception to active — meaning the CPU should take it now.
    // It does NOT return true just because active exceptions exist
    // (that would cause re-entry into the current handler).
    return updatePending();
}

int
MProfileSCS::acknowledgeIRQ()
{
    assert(!activeInterrupts.empty());
    return activeInterrupts.top()->interruptNum;
}

bool
MProfileSCS::canActivate(const Interrupt &intr) const
{
    // FAULTMASK: blocks everything except NMI (exc 2).
    if (faultmask && intr.interruptNum != 2)
        return false;

    // PRIMASK: blocks all configurable-priority exceptions (pri >= 0).
    if (primask && intr.priority >= 0)
        return false;

    // BASEPRI: blocks exceptions with priority >= basepri (when != 0).
    if (basepri != 0 && intr.priority >= basepri)
        return false;

    // Must have strictly higher priority (lower number) than the
    // currently active exception.  Check both the active queue
    // (normal activations via activateIRQ) and activePriorityCeiling
    // (set by SHCSR active bit writes for context-switch save/restore).
    int16_t ceiling = activePriorityCeiling;
    if (!activeInterrupts.empty()) {
        int16_t queueTop = activeInterrupts.top()->priority;
        if (queueTop < ceiling)
            ceiling = queueTop;
    }
    if (intr.priority >= ceiling)
        return false;

    return true;
}

void
MProfileSCS::pendInterrupt(Interrupt &intr)
{
    intr.pending = true;

    if (!intr.inPendingQueue) {
        pendingInterrupts.push(&intr);
        intr.inPendingQueue = true;
    }

    // Signal the CPU that an interrupt may be deliverable.
    // The CPU's checkInterrupts() will call hasDeliverableIRQ()
    // at the next instruction boundary.
    if (tc)
        tc->getCpuPtr()->postInterrupt(tc->threadId(), 0, 0);
}

bool
MProfileSCS::activateIRQ(int exc_num)
{
    // Validate exception number.
    if (exc_num < 0 || (uint32_t)exc_num >= interrupts.size())
        return false;

    Interrupt &intr = interrupts[exc_num];

    // Already active — nothing to do.
    if (intr.inActiveQueue)
        return false;

    // Must be enabled.  Disabled configurable faults should be
    // escalated to HardFault by the caller (m_faults.cc).
    if (!intr.enabled)
        return false;

    // Check if this exception can be delivered given the current
    // mask state and active exception priority.
    if (canActivate(intr)) {
        intr.active = true;
        activeInterrupts.push(&intr);
        intr.inActiveQueue = true;
        return true;
    }

    // Cannot activate — pend it.
    pendInterrupt(intr);
    return false;
}

void
MProfileSCS::deactivateIRQ(int exc_num)
{
    assert(!activeInterrupts.empty());
    Interrupt *top = activeInterrupts.top();
    assert(top->interruptNum == (uint32_t)exc_num);

    top->active = false;
    top->inActiveQueue = false;
    activeInterrupts.pop();

    // DDI0403E B1.5.8 DeActivate(): FAULTMASK is automatically
    // cleared to 0 on exception return, except when returning
    // from NMI (exc 2).  This ensures FAULTMASK doesn't persist
    // beyond the handler that set it.
    if (exc_num != 2 && hasBasePri)
        faultmask = false;
}

int16_t
MProfileSCS::getExcPriority(int exc_num) const
{
    if (exc_num < 0 || exc_num >= (int)interrupts.size())
        return 256;  // out of range → lowest priority
    return interrupts[exc_num].priority;
}

int16_t
MProfileSCS::getCurrentExcPriority() const
{
    if (activeInterrupts.empty())
        return 256;
    else
        return activeInterrupts.top()->priority;
}

int
MProfileSCS::getCurrentExcNum() const
{
    if (activeInterrupts.empty())
        return -1;
    else
        return activeInterrupts.top()->interruptNum;
}

bool
MProfileSCS::updatePending()
{
    // Clean stale entries from the pending queue and try to
    // activate the highest-priority valid pending exception.
    while (!pendingInterrupts.empty()) {
        Interrupt *top = pendingInterrupts.top();

        // Skip stale entries: disabled or no longer pending.
        if (!top->enabled || !top->pending) {
            top->inPendingQueue = false;
            pendingInterrupts.pop();
            continue;
        }

        // Found a valid pending entry — try to activate it.
        // canActivate checks masks and active queue priority.
        if (canActivate(*top)) {
            top->active = true;
            top->pending = false;
            top->inPendingQueue = false;
            pendingInterrupts.pop();
            activeInterrupts.push(top);
            top->inActiveQueue = true;
            return true;
        }

        // Top pending can't activate (blocked by mask or active
        // priority) — nothing below it in the queue can either,
        // since the queue is priority-ordered.
        break;
    }

    // No new exception was promoted.  Return false — the CPU
    // should NOT take an interrupt.  Active exceptions are already
    // being handled; returning true here would cause re-entry.
    return false;
}

// -- SysTick timer --

void
MProfileSCS::sysTickExpire(uint8_t index)
{
    SysTick &st = sysTicks[index];

    // Counter reached 0.  Set COUNTFLAG so firmware can detect
    // expiry by polling CSR (auto-clears on read per DDI0403E B3.3.1).
    st.ctrl |= (1u << 16);

    // If TICKINT (bit 1) is set, pend the SysTick exception (exc 15).
    // The CPU will pick it up at the next instruction boundary via
    // hasDeliverableIRQ() → updatePending().
    if (st.ctrl & (1u << 1)) {
        pendInterrupt(interrupts[15]);
    }

    // Reload and reschedule if timer is still enabled.
    // The counter wraps from 0 back to LOAD on each expiry.
    if (st.ctrl & 1)
        sysTickSchedule(index);
}

void
MProfileSCS::sysTickSchedule(uint8_t index)
{
    SysTick &st = sysTicks[index];

    // Don't schedule if LOAD is 0 — counter would never expire.
    if (st.load == 0)
        return;

    // Schedule expiry at curTick() + ((LOAD + 1) * clockPeriod).
    // The counter counts from LOAD down to 0 inclusive, so it takes
    // LOAD+1 cycles to expire.  Per DDI0403E B3.3.1: "the timer
    // counts down from the value in SYST_RVR to zero".
    Tick delay = clockPeriod() * ((Tick)st.load + 1);
    st.startTick = curTick();

    if (st.expireEvent.scheduled())
        deschedule(st.expireEvent);
    schedule(st.expireEvent, curTick() + delay);
}

uint32_t
MProfileSCS::sysTickCurrentValue(uint8_t index) const
{
    const SysTick &st = sysTicks[index];

    // Current value is computed from how much time remains until
    // the scheduled expiry event.  If the timer isn't running,
    // return 0 (DDI0403E B3.3.5: CVR reads as UNKNOWN when
    // disabled; returning 0 is safe).
    if (!st.expireEvent.scheduled())
        return 0;

    // Guard against the case where curTick has passed the event
    // time (possible due to scheduling granularity).
    if (curTick() >= st.expireEvent.when())
        return 0;

    Tick remaining = st.expireEvent.when() - curTick();
    return (uint32_t)(remaining / clockPeriod());
}

// -- Checkpoint --

void
MProfileSCS::serialize(CheckpointOut &cp) const
{
    // -- Interrupt state --
    // Save per-interrupt fields individually.  Priority queues are
    // not saved — rebuilt from interrupt state on restore.
    uint32_t numInterrupts = interrupts.size();
    SERIALIZE_SCALAR(numInterrupts);

    for (uint32_t i = 0; i < numInterrupts; ++i) {
        ScopedCheckpointSection sec(cp, csprintf("interrupt%d", i));
        const Interrupt &intr = interrupts[i];
        SERIALIZE_SCALAR(intr.active);
        SERIALIZE_SCALAR(intr.enabled);
        SERIALIZE_SCALAR(intr.pending);
        SERIALIZE_SCALAR(intr.priority);
        SERIALIZE_SCALAR(intr.interruptNum);
    }

    // -- Mask state --
    {
        ScopedCheckpointSection sec(cp, "maskState");
        SERIALIZE_SCALAR(primask);
        SERIALIZE_SCALAR(faultmask);
        SERIALIZE_SCALAR(basepri);
        SERIALIZE_SCALAR(activePriorityCeiling);
    }

    // -- SCB registers stored locally --
    // VTOR, AIRCR, SCR, CCR are in MISA misc regs — serialized
    // by MISA.  Only SCS-local registers saved here.
    {
        ScopedCheckpointSection sec(cp, "scbRegs");
        SERIALIZE_SCALAR(cfsr);
        SERIALIZE_SCALAR(hfsr);
        SERIALIZE_SCALAR(dfsr);
        SERIALIZE_SCALAR(mmfar);
        SERIALIZE_SCALAR(bfar);
    }

    // -- SysTick state --
    for (uint8_t i = 0; i < sysTicks.size(); ++i) {
        ScopedCheckpointSection sec(cp, csprintf("sysTick%d", i));
        const SysTick &st = sysTicks[i];
        SERIALIZE_SCALAR(st.ctrl);
        SERIALIZE_SCALAR(st.load);
        SERIALIZE_SCALAR(st.startTick);

        bool eventScheduled = st.expireEvent.scheduled();
        SERIALIZE_SCALAR(eventScheduled);
        if (eventScheduled) {
            Tick eventTime = st.expireEvent.when();
            SERIALIZE_SCALAR(eventTime);
        }
    }
}

void
MProfileSCS::unserialize(CheckpointIn &cp)
{
    // -- Interrupt state --
    uint32_t numInterrupts;
    UNSERIALIZE_SCALAR(numInterrupts);
    assert(numInterrupts == interrupts.size());

    for (uint32_t i = 0; i < numInterrupts; ++i) {
        ScopedCheckpointSection sec(cp, csprintf("interrupt%d", i));
        Interrupt &intr = interrupts[i];
        UNSERIALIZE_SCALAR(intr.active);
        UNSERIALIZE_SCALAR(intr.enabled);
        UNSERIALIZE_SCALAR(intr.pending);
        UNSERIALIZE_SCALAR(intr.priority);
        UNSERIALIZE_SCALAR(intr.interruptNum);
        intr.inPendingQueue = false;
        intr.inActiveQueue = false;
    }

    // -- Mask state --
    {
        ScopedCheckpointSection sec(cp, "maskState");
        UNSERIALIZE_SCALAR(primask);
        UNSERIALIZE_SCALAR(faultmask);
        UNSERIALIZE_SCALAR(basepri);
        UNSERIALIZE_SCALAR(activePriorityCeiling);
    }

    // -- SCB registers stored locally --
    {
        ScopedCheckpointSection sec(cp, "scbRegs");
        UNSERIALIZE_SCALAR(cfsr);
        UNSERIALIZE_SCALAR(hfsr);
        UNSERIALIZE_SCALAR(dfsr);
        UNSERIALIZE_SCALAR(mmfar);
        UNSERIALIZE_SCALAR(bfar);
    }

    // -- SysTick state --
    for (uint8_t i = 0; i < sysTicks.size(); ++i) {
        ScopedCheckpointSection sec(cp, csprintf("sysTick%d", i));
        SysTick &st = sysTicks[i];
        UNSERIALIZE_SCALAR(st.ctrl);
        UNSERIALIZE_SCALAR(st.load);
        UNSERIALIZE_SCALAR(st.startTick);

        bool eventScheduled;
        UNSERIALIZE_SCALAR(eventScheduled);
        if (eventScheduled) {
            Tick eventTime;
            UNSERIALIZE_SCALAR(eventTime);
            schedule(st.expireEvent, eventTime);
        }
    }

    // Rebuild priority queues from deserialized interrupt state.
    // Cannot call updatePending() here because tc is not set until
    // startup().  gem5 restore order: ctor → init → unserialize →
    // startup.
    pendingInterrupts = {};
    activeInterrupts = {};
    for (uint32_t i = 0; i < interrupts.size(); ++i) {
        Interrupt &intr = interrupts[i];
        if (intr.pending && intr.enabled) {
            pendingInterrupts.push(&intr);
            intr.inPendingQueue = true;
        }
        if (intr.active) {
            activeInterrupts.push(&intr);
            intr.inActiveQueue = true;
        }
    }
}

} // namespace gem5
