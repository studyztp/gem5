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

#ifndef __ARCH_ARM_M_ISA_HH__
#define __ARCH_ARM_M_ISA_HH__

/** @file
 * M-profile ISA (ARMv6-M / ARMv7-M / ARMv7E-M).
 *
 * Inherits directly from BaseISA (not from the A-profile ISA class).
 * This is the same pattern used by RISC-V (src/arch/riscv/isa.hh).
 *
 * == Why not inherit from A-profile ISA? ==
 *
 * The A-profile ISA constructor (isa.cc:89-138) calls
 * initializeMiscRegMetadata() which initializes 750+ A-profile
 * registers, calls resetCPSR(system) that dereferences an ArmSystem
 * pointer (NULL for M-profile → segfault), creates SelfDebug, and
 * runs preUnflattenMiscReg().  All A-profile specific and harmful
 * for M-profile.
 *
 * M-profile has only ~24 misc registers (vs 750+), no CPSR (uses
 * xPSR), no exception levels, no register banking, no SPSR, no
 * SelfDebug.  It's fundamentally simpler and deserves its own ISA.
 *
 * == CPSR aliasing ==
 *
 * Thumb instructions that fall through the standard decoder may
 * access MISCREG_CPSR (e.g., loads do cSwap(Mem, Cpsr.e) for
 * endianness).  M-profile has no CPSR — readMiscReg(MISCREG_CPSR)
 * constructs a CPSR-compatible value from xPSR.
 * See step8_cpsr_alias_proof.md for the exhaustive safety proof.
 *
 * == Register array ==
 *
 * Uses the full NUM_MISCREGS array size from misc.hh so that
 * A-profile MISCREG indices (MISCREG_CPSR=0, etc.) are valid
 * array indices.  Only ~24 entries are actively used.
 *
 * References:
 *   DDI0403E — ARMv7-M Architecture Reference Manual
 *   DDI0419  — ARMv6-M Architecture Reference Manual
 *   src/arch/riscv/isa.hh — BaseISA inheritance pattern
 */

#include "arch/arm/pcstate.hh"
#include "arch/arm/regs/misc.hh"
#include "arch/arm/types.hh"
#include "arch/generic/isa.hh"
#include "enums/ArmExtension.hh"
#include "params/ArmMISA.hh"

namespace gem5
{

class ArmRelease;
class ArmMSystem;

namespace ArmISA
{

class MISA : public BaseISA
{
  protected:
    // -- Misc register storage --
    // Full NUM_MISCREGS size so that A-profile indices (MISCREG_CPSR=0,
    // MISCREG_CPSR_Q, etc.) are valid.  Only ~24 M-profile entries
    // are actively used; CPSR reads/writes are aliased to xPSR.
    RegVal miscRegs[NUM_MISCREGS];

    // -- Configuration --

    /** VTOR alignment mask, computed from vtor_align_bits param.
     *  Applied on every write to MISCREG_M_VTOR.
     *  Example: vtor_align_bits=9 → mask=0xFFFFFE00 (M4/M7).
     */
    uint32_t vtorAlignMask;

    /** Pointer to the M-profile system (for extension checks). */
    ArmMSystem *mSystem = nullptr;

    /** ARM release for extension checking (M_PROFILE, ARMV7M, etc.). */
    const ArmRelease *release = nullptr;

    // -- Exclusive monitor for LDREX/STREX --
    static constexpr Addr INVALID_LOCK_ADDR = (Addr)-1;
    Addr lockedAddr = INVALID_LOCK_ADDR;

  public:
    PARAMS(ArmMISA);
    MISA(const Params &p);

    // ================================================================
    // BaseISA pure virtual implementations
    // ================================================================

    /** Create a new ARM PCState (with Thumb bit). */
    PCStateBase *
    newPCState(Addr new_inst_addr=0) const override
    {
        return new PCState(new_inst_addr);
    }

    /** Reset all M-profile misc regs to documented default values.
     *  DDI0403E B1.4.2 (reset values). */
    void clear() override;

    /** Read misc reg without side effects (raw array access). */
    RegVal readMiscRegNoEffect(RegIndex idx) const override;

    /** Read misc reg with side effects.
     *  Handles xPSR T-bit sync, BASEPRI_MAX alias, CPSR alias. */
    RegVal readMiscReg(RegIndex idx) override;

    /** Write misc reg without side effects (raw array access). */
    void setMiscRegNoEffect(RegIndex idx, RegVal val) override;

    /** Write misc reg with side effects.
     *  Handles VTOR alignment, AIRCR VECTKEY, W1C, CPSR_Q alias. */
    void setMiscReg(RegIndex idx, RegVal val) override;

    /** M-profile privilege: check CONTROL.nPRIV.
     *  0 = privileged (Thread mode default), 1 = unprivileged.
     *  DDI0403E B1.4.4. */
    bool inUserMode() const override;

    /** Copy all registers from another ThreadContext. */
    void copyRegsFrom(ThreadContext *src) override;

    // ================================================================
    // Optional BaseISA overrides
    // ================================================================

    void serialize(CheckpointOut &cp) const override;
    void unserialize(CheckpointIn &cp) override;

    /** LDREX: record exclusive address. */
    void handleLockedRead(const RequestPtr &req) override;

    /** STREX: check exclusive address, clear monitor. */
    bool handleLockedWrite(const RequestPtr &req,
                           Addr cacheBlockMask) override;

    /** CLREX / context switch: clear exclusive monitor. */
    void globalClearExclusive() override;

    // ================================================================
    // M-profile specific
    // ================================================================

    /** Get the ARM release for extension checking. */
    const ArmRelease *getRelease() const { return release; }

    /** Check if a specific ARM extension is present. */
    bool has(ArmExtension ext) const;
};

} // namespace ArmISA
} // namespace gem5

#endif // __ARCH_ARM_M_ISA_HH__
