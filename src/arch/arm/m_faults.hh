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

#ifndef __ARCH_ARM_M_FAULTS_HH__
#define __ARCH_ARM_M_FAULTS_HH__

/**
 * @file
 * M-profile (ARMv7-M / ARMv8-M) exception and fault model.
 *
 * M-profile exception entry is architecturally different from A/R-profile.
 * On A/R-profile the CPU switches to a banked register set and the handler
 * address is an instruction at a fixed offset from VBAR.  On M-profile the
 * hardware pushes an 8-register frame to the current stack, sets LR to a
 * magic EXC_RETURN value, and reads the handler address from the VTOR-based
 * vector table.  There is no CPSR mode switch, no SPSR, and no banked
 * registers.
 *
 * Because virtually nothing in ArmFault (SPSR save, CPSR mode switch,
 * VBAR+offset vectors, EL routing, etc.) applies to M-profile, ArmMFault
 * inherits directly from FaultBase.  The CPU pipeline interacts with
 * faults exclusively through the FaultBase interface (name() and invoke()),
 * so this integrates transparently with all CPU models.
 */

#include "cpu/null_static_inst.hh"
#include "sim/faults.hh"

namespace gem5
{

namespace ArmISA
{

/**
 * M-profile exception numbers (DDI0403E Table B1-4).
 *
 * Used to index into the VTOR-based vector table:
 *   handler address = mem[VTOR + 4 * exception_number]
 *
 * Also written into xPSR.IPSR on exception entry to identify the
 * active exception (IPSR == 0 means Thread mode).
 */
enum MProfileExcNum : int
{
    MPEXC_RESET         = 1,
    MPEXC_NMI           = 2,
    MPEXC_HARDFAULT     = 3,
    MPEXC_MEMMANAGE     = 4,
    MPEXC_BUSFAULT      = 5,
    MPEXC_USAGEFAULT    = 6,
    // 7-10 reserved
    MPEXC_SVCALL        = 11,
    MPEXC_DEBUGMON      = 12,
    // 13 reserved
    MPEXC_PENDSV        = 14,
    MPEXC_SYSTICK       = 15,
    MPEXC_EXTERNAL_BASE = 16,
};

/**
 * Map M-profile exception number to its SHCSR active bit position.
 * Returns -1 if the exception has no SHCSR active bit (HardFault, NMI,
 * Reset, or external IRQs — external IRQs use nvicActive[] instead).
 * DDI0403E B3.2.10: SHCSR active bit positions.
 */
inline int mProfileShcsrActiveBit(int exc_num)
{
    switch (exc_num) {
        case MPEXC_MEMMANAGE:  return 0;   // MEMFAULTACT
        case MPEXC_BUSFAULT:   return 1;   // BUSFAULTACT
        case MPEXC_USAGEFAULT: return 3;   // USGFAULTACT
        case MPEXC_SVCALL:     return 7;   // SVCALLACT
        case MPEXC_DEBUGMON:   return 8;   // MONITORACT
        case MPEXC_PENDSV:     return 10;  // PENDSVACT
        case MPEXC_SYSTICK:    return 11;  // SYSTICKACT
        default:               return -1;  // no SHCSR active bit
    }
}

/**
 * Base class for all M-profile exceptions.
 *
 * A single ArmMFault instance can represent any non-Reset exception;
 * the exception number passed to the constructor selects the fault
 * type, vector table entry, and entry behaviour.
 *
 * MProfileReset is the only subclass because its invoke() is
 * fundamentally different (reads initial MSP / PC from the vector
 * table instead of pushing a frame).
 *
 * Usage at creation sites:
 *   return std::make_shared<ArmMFault>(MPEXC_SVCALL);
 *   return std::make_shared<ArmMFault>(MPEXC_HARDFAULT);
 *   return std::make_shared<ArmMFault>(MPEXC_EXTERNAL_BASE + irq_num);
 *   return std::make_shared<MProfileReset>();
 */
class ArmMFault : public FaultBase
{
  protected:
    const int _excNumber;

  public:
    ArmMFault(int exc_num) : _excNumber(exc_num) {}

    FaultName name() const override;
    void invoke(ThreadContext *tc,
                const StaticInstPtr &inst = nullStaticInstPtr) override;

    int excNumber() const { return _excNumber; }

  protected:
    /**
     * Return a human-readable name for the given exception number.
     * Used by name() for debug tracing / logging.
     */
    static const char *excName(int exc_num);

    /**
     * Return true if the stacked return address should be advanced
     * past the current instruction (PC + inst_size).
     *
     * True only for SVCall — the caller does not want to re-execute
     * the SVC instruction after the handler returns.
     *
     * False for faults (return to the faulting instruction for retry)
     * and for asynchronous exceptions (the PC already points to the
     * next instruction to execute).
     */
    static bool excAdvancesPC(int exc_num);

    /**
     * Push the 8-word exception frame onto the given stack.
     *
     * Frame layout per DDI0403E B1.5.6 (addresses relative to the
     * new SP after decrement):
     *
     *   SP+0x00: R0        SP+0x10: R12
     *   SP+0x04: R1        SP+0x14: LR  (pre-exception R14)
     *   SP+0x08: R2        SP+0x18: ReturnAddress (stacked PC)
     *   SP+0x0C: R3        SP+0x1C: xPSR (with frameptralign in bit 9)
     *
     * @param tc        Thread context.
     * @param inst      Faulting instruction (may be null for async).
     * @param sp        Current stack pointer value.
     * @param stkalign  True if CCR.STKALIGN is set.
     * @return  The new stack pointer (frame base after the push).
     */
    uint32_t pushExceptionFrame(ThreadContext *tc,
                                const StaticInstPtr &inst,
                                uint32_t sp, bool stkalign);

    /** Read the handler address from VTOR + 4 * _excNumber. */
    Addr getHandlerAddress(ThreadContext *tc) const;

    /**
     * Compute the EXC_RETURN magic value for LR.
     *
     * Encodes the pre-exception execution state so that exception
     * return (BX LR) knows which stack to unstack from and which
     * mode to restore:
     *   0xFFFFFFF1 — return to Handler mode, use MSP
     *   0xFFFFFFF9 — return to Thread mode,  use MSP
     *   0xFFFFFFFD — return to Thread mode,  use PSP
     *
     * @param was_handler  True if the exception was taken from Handler mode.
     * @param used_psp     True if PSP was the active stack when the
     *                     exception was taken.
     */
    static uint32_t computeExcReturn(bool was_handler, bool used_psp);
};

/**
 * M-profile Reset.
 *
 * Reset reads the initial MSP from VTOR+0 and the Reset_Handler
 * address from VTOR+4, then sets xPSR to its reset value.
 * It does NOT push an exception frame or set EXC_RETURN.
 */
class MProfileReset : public ArmMFault
{
  protected:
    /** Vector table address to use after clearArchRegs().
     *  On real hardware VTOR resets to 0 (boot alias maps flash there).
     *  In gem5, the workload sets this to the ELF's flash base address
     *  since we may not have a boot alias at address 0. */
    uint32_t vtorAddr;

  public:
    MProfileReset(uint32_t _vtor = 0) : ArmMFault(MPEXC_RESET),
        vtorAddr(_vtor) {}
    void invoke(ThreadContext *tc,
                const StaticInstPtr &inst = nullStaticInstPtr) override;
};

/**
 * Perform M-profile exception return (unstack).
 *
 * Called when a branch target is detected as an EXC_RETURN value.
 * Detection condition: (new_pc & 0xFFFFFFF0) == 0xFFFFFFF0
 *
 * Pops the 8-word exception frame from the appropriate stack (MSP or
 * PSP, as encoded in the EXC_RETURN value), restores registers and
 * xPSR, and sets PC to the stacked return address.
 *
 * Note: the hook that detects EXC_RETURN in the PC-write / branch
 * path belongs to Step 8 (decoder adjustments).  This function
 * provides the unstacking logic to be called from that hook.
 *
 * @param tc         Thread context.
 * @param exc_return The EXC_RETURN value that was loaded into PC.
 */
void mProfileExcReturnUnstack(ThreadContext *tc, uint32_t exc_return);

// =========================================================================
// CC flat register ↔ xPSR NZCV/GE sync helpers (BUG-5)
// =========================================================================
//
// gem5 reuses A-profile Thumb instruction implementations for M-profile.
// A-profile ALU instructions store NZCV and GE flags in CC flat registers
// (cc_reg::Nz, C, V, Ge), NOT in MISCREG_M_XPSR.  This means xPSR's
// NZCV/GE bits [31:28,19:16] are always stale with respect to the last
// ALU instruction.
//
// These helpers sync between the two representations at boundaries where
// both must agree:
//
//   syncCCRegsToXpsr():  CC flat regs → MISCREG_M_XPSR
//     Used before: exception frame stacking, MRS APSR read, serialize
//
//   syncXpsrToCCRegs():  MISCREG_M_XPSR → CC flat regs
//     Used after:  exception frame unstacking, MSR APSR write, unserialize

/** Sync live CC flat registers into MISCREG_M_XPSR (NZCV + GE bits). */
void syncCCRegsToXpsr(ThreadContext *tc);

/** Sync MISCREG_M_XPSR NZCV + GE bits into CC flat registers. */
void syncXpsrToCCRegs(ThreadContext *tc);

} // namespace ArmISA
} // namespace gem5

#endif // __ARCH_ARM_M_FAULTS_HH__
