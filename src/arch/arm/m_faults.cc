/*
 * Copyright (c) 2026 The gem5 Contributors
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

#include "arch/arm/pcstate.hh"
#include "arch/arm/regs/int.hh"
#include "arch/arm/regs/misc.hh"
#include "arch/arm/regs/misc_types.hh"
#include "arch/arm/system.hh"
#include "base/trace.hh"
#include "cpu/base.hh"
#include "cpu/static_inst.hh"
#include "cpu/thread_context.hh"
#include "debug/Faults.hh"
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

    // Build the stacked xPSR — sync T-bit from PCState.
    XPSR xpsr = tc->readMiscRegNoEffect(MISCREG_M_XPSR);
    xpsr.t = pcState.thumb() ? 1 : 0;

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

    // Write the frame to physical memory.
    // TODO: M-profile exception stacking goes through the MPU on real
    // hardware (a MemManage fault can occur during stacking).  We
    // bypass the MPU here because it is not yet modelled.  Revisit
    // when the MPU is implemented.
    PortProxy &phys = tc->getSystemPtr()->physProxy;
    phys.write<uint32_t>(frameptr + 0x00,
                         (uint32_t)tc->getReg(int_reg::R0),
                         ByteOrder::little);
    phys.write<uint32_t>(frameptr + 0x04,
                         (uint32_t)tc->getReg(int_reg::R1),
                         ByteOrder::little);
    phys.write<uint32_t>(frameptr + 0x08,
                         (uint32_t)tc->getReg(int_reg::R2),
                         ByteOrder::little);
    phys.write<uint32_t>(frameptr + 0x0C,
                         (uint32_t)tc->getReg(int_reg::R3),
                         ByteOrder::little);
    phys.write<uint32_t>(frameptr + 0x10,
                         (uint32_t)tc->getReg(int_reg::R12),
                         ByteOrder::little);
    phys.write<uint32_t>(frameptr + 0x14,
                         (uint32_t)tc->getReg(int_reg::Lr),
                         ByteOrder::little);
    phys.write<uint32_t>(frameptr + 0x18,
                         (uint32_t)returnAddr,
                         ByteOrder::little);
    phys.write<uint32_t>(frameptr + 0x1C,
                         (uint32_t)xpsr,
                         ByteOrder::little);

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

    // ---- 1. Determine pre-exception state ----

    XPSR xpsr = tc->readMiscRegNoEffect(MISCREG_M_XPSR);
    bool inHandler = (xpsr.exception != 0);

    // Handler mode always uses MSP.
    // Thread mode uses MSP (CONTROL.SPSEL=0) or PSP (CONTROL.SPSEL=1).
    CONTROL_M ctrl = tc->readMiscRegNoEffect(MISCREG_M_CONTROL);
    bool usePSP = !inHandler && ctrl.spsel;

    // TODO: M-profile has two stack pointers (MSP and PSP) stored as
    // misc regs.  The mapping between architectural R13 and MSP/PSP
    // is not yet wired through the register flattening system.
    // Currently we access them via MISCREG directly.  Revisit when
    // CONTROL.SPSEL switching is implemented (see Step 3 deferral).
    MiscRegIndex spReg = usePSP ? MISCREG_M_PSP : MISCREG_M_MSP;
    uint32_t sp = tc->readMiscRegNoEffect(spReg);

    // ---- 2. Push exception frame ----

    CCR_t ccr = tc->readMiscRegNoEffect(MISCREG_M_CCR);
    uint32_t newSP = pushExceptionFrame(tc, inst, sp, ccr.stkalign);

    // Write back the stack pointer that was used for stacking.
    tc->setMiscRegNoEffect(spReg, newSP);

    // ---- 3. Set LR to EXC_RETURN ----

    uint32_t excReturn = computeExcReturn(inHandler, usePSP);
    tc->setReg(int_reg::Lr, (RegVal)excReturn);

    // ---- 4. Read handler address from VTOR vector table ----

    Addr handlerAddr = getHandlerAddress(tc);

    // ---- 5. Enter Handler mode ----

    // Set xPSR.IPSR = exception number (non-zero → Handler mode).
    // Clear ICI/IT state (DDI0403E B1.5.6).
    xpsr.exception = _excNumber;
    xpsr.iciIt1 = 0;
    xpsr.iciIt2 = 0;
    tc->setMiscRegNoEffect(MISCREG_M_XPSR, xpsr);

    // ---- 6. Branch to handler ----

    // Bit[0] of the vector table entry indicates Thumb state (must be
    // 1 for M-profile).  The actual branch target has bit[0] cleared.
    // TODO: The Thumb state assumption (pc.thumb(true)) depends on
    // the gem5 Thumb/Thumb-2 decoder being complete enough for
    // M-profile instructions.  An audit of thumb.isa coverage
    // (especially 32-bit Thumb-2 entries and their interaction with
    // arm.isa implementations) is required in Step 8.  If gaps are
    // found, either the Thumb decoder must be extended or an
    // alternative decoding strategy is needed.
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

    // Clear interrupts and architectural state (same as A-profile Reset).
    tc->getCpuPtr()->clearInterrupts(tc->threadId());
    tc->clearArchRegs();

    // VTOR resets to 0 — the vector table sits at address 0x00000000.
    uint32_t vtor = tc->readMiscRegNoEffect(MISCREG_M_VTOR);
    PortProxy &phys = tc->getSystemPtr()->physProxy;

    // Entry 0: initial Main Stack Pointer value.
    uint32_t initialMSP = phys.read<uint32_t>(vtor, ByteOrder::little);
    // Entry 1: Reset_Handler address.
    uint32_t resetHandler = phys.read<uint32_t>(vtor + 4,
                                                ByteOrder::little);

    // Initialise MSP.
    tc->setMiscRegNoEffect(MISCREG_M_MSP, initialMSP);

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
// mProfileExcReturn — M-profile exception return (unstack)
// ---------------------------------------------------------------------------

void
mProfileExcReturn(ThreadContext *tc, uint32_t exc_return)
{
    // Decode EXC_RETURN (DDI0403E B1.5.8).
    //   bit[3] = 0: return to Handler mode, 1: return to Thread mode
    //   bit[2] = 0: restore from MSP,       1: restore from PSP
    bool returnToThread = (exc_return & 0x8);
    bool restoreFromPSP = (exc_return & 0x4);

    // TODO: Same MSP/PSP misc-reg access note as in ArmMFault::invoke()
    // — R13 flattening is not yet wired for M-profile.
    MiscRegIndex spReg = restoreFromPSP ? MISCREG_M_PSP : MISCREG_M_MSP;
    uint32_t frameptr = tc->readMiscRegNoEffect(spReg);

    // Read the exception frame from physical memory.
    // TODO: Unstacking goes through the MPU on real hardware.
    // Bypassed here because the MPU is not yet modelled.
    PortProxy &phys = tc->getSystemPtr()->physProxy;
    uint32_t r0      = phys.read<uint32_t>(frameptr + 0x00,
                                           ByteOrder::little);
    uint32_t r1      = phys.read<uint32_t>(frameptr + 0x04,
                                           ByteOrder::little);
    uint32_t r2      = phys.read<uint32_t>(frameptr + 0x08,
                                           ByteOrder::little);
    uint32_t r3      = phys.read<uint32_t>(frameptr + 0x0C,
                                           ByteOrder::little);
    uint32_t r12     = phys.read<uint32_t>(frameptr + 0x10,
                                           ByteOrder::little);
    uint32_t lr      = phys.read<uint32_t>(frameptr + 0x14,
                                           ByteOrder::little);
    uint32_t retAddr = phys.read<uint32_t>(frameptr + 0x18,
                                           ByteOrder::little);
    XPSR xpsr        = phys.read<uint32_t>(frameptr + 0x1C,
                                           ByteOrder::little);

    // Restore general-purpose registers.
    tc->setReg(int_reg::R0,  (RegVal)r0);
    tc->setReg(int_reg::R1,  (RegVal)r1);
    tc->setReg(int_reg::R2,  (RegVal)r2);
    tc->setReg(int_reg::R3,  (RegVal)r3);
    tc->setReg(int_reg::R12, (RegVal)r12);
    tc->setReg(int_reg::Lr,  (RegVal)lr);

    // Restore SP — undo alignment padding if frameptralign was set.
    uint32_t sp = frameptr + 0x20;
    if (xpsr.frameptralign)
        sp += 4;
    tc->setMiscRegNoEffect(spReg, sp);

    // Restore xPSR.  The stacked value already contains the correct
    // IPSR for the interrupted context (0 for Thread mode, non-zero
    // for a nested Handler).  Clear frameptralign since it is only
    // meaningful in the stacked copy.
    xpsr.frameptralign = 0;
    tc->setMiscRegNoEffect(MISCREG_M_XPSR, xpsr);

    // Set PC to the stacked return address.
    // TODO: Same Thumb decoder dependency — see ArmMFault::invoke().
    PCState pc(retAddr & ~0x1);
    pc.thumb(true);
    pc.nextThumb(true);
    pc.aarch64(false);
    pc.nextAArch64(false);
    pc.illegalExec(false);
    tc->pcState(pc);

    DPRINTF(Faults, "M-profile exception return: EXC_RETURN=%#x "
            "retAddr=%#x SP=%#x %s mode\n",
            exc_return, retAddr & ~0x1, sp,
            returnToThread ? "Thread" : "Handler");
}

} // namespace ArmISA
} // namespace gem5
