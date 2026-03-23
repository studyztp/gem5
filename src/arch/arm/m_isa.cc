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
 * M-profile ISA implementation (BaseISA subclass).
 *
 * This file contains the complete ISA implementation for ARM M-profile
 * (Cortex-M0 through Cortex-M7).  It does NOT inherit from or call
 * any A-profile ISA code.  All register initialization, read/write
 * handling, and CPSR aliasing is self-contained here.
 */

#include "arch/arm/m_isa.hh"

#include <cstring>

#include "arch/arm/m_faults.hh"
#include "arch/arm/m_system.hh"
#include "arch/arm/pcstate.hh"
#include "arch/arm/regs/cc.hh"
#include "arch/arm/regs/int.hh"
#include "arch/arm/regs/mat.hh"
#include "arch/arm/regs/misc.hh"
#include "arch/arm/regs/misc_info.hh"
#include "arch/arm/regs/misc_types.hh"
#include "arch/arm/regs/vec.hh"
#include "arch/arm/system.hh"
#include "base/bitfield.hh"
#include "base/trace.hh"
#include "cpu/base.hh"
#include "cpu/thread_context.hh"
#include "debug/FloatRegs.hh"
#include "debug/MProfileCPSR.hh"
#include "mem/request.hh"

namespace gem5
{

namespace
{

// ARM uses vector registers for FP, not a separate float reg class.
// Define a zero-size placeholder (same as isa.cc:84).
RegClass floatRegClass(FloatRegClass, FloatRegClassName, 0, debug::FloatRegs);

} // anonymous namespace

namespace ArmISA
{

// M-profile BitUnion types live in namespace ArmMISA (misc_types.hh).
using namespace ArmMISA;

// =========================================================================
// Constructor
// =========================================================================

MISA::MISA(const Params &p)
    : BaseISA(p, "arm_m"),
      vtorAlignMask(0)
{
    // -- Register classes --
    // M-profile shares ARM's register file layout.
    // Use flatIntRegClass (not intRegClass) because M-profile has
    // NO register banking — R0-R14 are always the same physical regs
    // (unlike A-profile which has banked R8-R14 for FIQ/IRQ/etc.).
    _regClasses.push_back(&flatIntRegClass);
    _regClasses.push_back(&floatRegClass);
    _regClasses.push_back(&vecRegClass);
    _regClasses.push_back(&vecElemClass);
    _regClasses.push_back(&vecPredRegClass);
    _regClasses.push_back(&matRegClass);
    _regClasses.push_back(&ccRegClass);
    _regClasses.push_back(&miscRegClass);

    // -- VTOR alignment mask --
    // Validate and compute from vtor_align_bits param.
    // DDI0403E B3.2.5: VTOR bits[N-1:0] are RES0.
    fatal_if(p.vtor_align_bits < 7 || p.vtor_align_bits > 31,
             "ArmMISA: vtor_align_bits=%u out of range [7,31].",
             (unsigned)p.vtor_align_bits);
    vtorAlignMask = ~((1u << p.vtor_align_bits) - 1u);

    // -- System and release --
    // Try to get ArmMSystem for extension checking.
    // During ISA construction, p.system may be the parent System.
    // ArmMSystem provides releaseFS() for M-profile ArmRelease.
    mSystem = dynamic_cast<ArmMSystem *>(p.system);
    if (mSystem) {
        release = mSystem->releaseFS();
    } else {
        // SE mode or system not yet available — use param.
        release = p.release_se;
    }

    // -- Initialize misc regs --
    std::memset(miscRegs, 0, sizeof(miscRegs));
    clear();
}

// =========================================================================
// clear() — reset all M-profile misc regs to documented defaults
// =========================================================================
//
// Reset values from DDI0403E B1.4.2 and B3.2.
// Only the ~24 M-profile registers are initialized.
// All other entries (including MISCREG_CPSR at index 0) stay zero.

void
MISA::clear()
{
    // Zero everything first
    std::memset(miscRegs, 0, sizeof(miscRegs));

    // -- Core special registers (DDI0403E B1.4.2) --
    // xPSR: T-bit set (bit 24) = Thumb mode, IPSR=0 = Thread mode
    miscRegs[MISCREG_M_XPSR] = 0x01000000;
    // MSP/PSP: loaded from vector table at reset (0 until then)
    miscRegs[MISCREG_M_MSP] = 0;
    miscRegs[MISCREG_M_PSP] = 0;
    // CONTROL: nPRIV=0 (privileged), SPSEL=0 (MSP), FPCA=0
    miscRegs[MISCREG_M_CONTROL] = 0;
    // Interrupt masks: all disabled (interrupts enabled)
    miscRegs[MISCREG_M_PRIMASK] = 0;
    miscRegs[MISCREG_M_BASEPRI] = 0;
    miscRegs[MISCREG_M_FAULTMASK] = 0;

    // -- SCB registers (DDI0403E B3.2) --
    // BUG-3 fix: CPUID value comes from ArmMSystem (set by platform),
    // not hardcoded.  This allows different core variants (M0/M3/M4/M7)
    // to report the correct CPUID to firmware via SCB->CPUID (0xE000ED00).
    miscRegs[MISCREG_M_CPUID] = mSystem->getCPUID();
    // ICSR is not modeled as a misc reg — computed on-the-fly by
    // MProfileSCS from xPSR.IPSR, internal SCS state, and SHCSR.
    // VTOR: 0 at reset (vector table at address 0)
    miscRegs[MISCREG_M_VTOR] = 0;
    // AIRCR: VECTKEYSTAT=0xFA05 in readback, PRIGROUP=0
    miscRegs[MISCREG_M_AIRCR] = 0xFA050000;
    // SCR: all features disabled at reset
    miscRegs[MISCREG_M_SCR] = 0;
    // CCR: STKALIGN=1 (bit 9) — 8-byte stack alignment on exception
    miscRegs[MISCREG_M_CCR] = 0x200;
    // SHPR1/2/3: all system handler priorities = 0 (highest)
    miscRegs[MISCREG_M_SHPR1] = 0;
    miscRegs[MISCREG_M_SHPR2] = 0;
    miscRegs[MISCREG_M_SHPR3] = 0;
    // SHCSR: no faults enabled or active
    miscRegs[MISCREG_M_SHCSR] = 0;
    // Fault status registers: all clear
    miscRegs[MISCREG_M_CFSR] = 0;
    miscRegs[MISCREG_M_HFSR] = 0;
    miscRegs[MISCREG_M_DFSR] = 0;
    miscRegs[MISCREG_M_MMFAR] = 0;
    miscRegs[MISCREG_M_BFAR] = 0;
    miscRegs[MISCREG_M_AFSR] = 0;
}

// =========================================================================
// readMiscRegNoEffect — raw array access (no side effects)
// =========================================================================

RegVal
MISA::readMiscRegNoEffect(RegIndex idx) const
{
    if (idx >= NUM_MISCREGS) {
        warn("MISA::readMiscRegNoEffect: index %d out of range", idx);
        return 0;
    }
    return miscRegs[idx];
}

// =========================================================================
// readMiscReg — with side effects and aliasing
// =========================================================================

RegVal
MISA::readMiscReg(RegIndex idx)
{
    switch (idx) {
      case MISCREG_M_XPSR: {
          // T-bit (bit 24) reflects Thumb state from PCState,
          // not the stored value.  DDI0403E B1.4.2.
          // Analogous to A-profile CPSR.T sync (isa.cc:461-465).
          XPSR xpsr = miscRegs[MISCREG_M_XPSR];
          auto pc = tc->pcState().as<PCState>();
          xpsr.t = pc.thumb() ? 1 : 0;
          return xpsr;
      }

      case MISCREG_M_BASEPRI_MAX:
          // Reads are UNPREDICTABLE per spec; real M4 silicon returns
          // BASEPRI.  Match that for MRS round-trip correctness.
          return miscRegs[MISCREG_M_BASEPRI];

      // =============================================================
      // CPSR → xPSR alias
      // =============================================================
      //
      // M-profile has NO CPSR.  But Thumb instructions that fall
      // through the standard decoder access MISCREG_CPSR (operands.isa
      // maps Cpsr → MISCREG_CPSR at index 0).
      //
      // After MDecoder intercepts all dangerous instructions, the
      // ONLY CPSR field read by fall-through instructions is bit[9]
      // (E = endianness) in cSwap() calls.  We construct a
      // CPSR-compatible value from xPSR.
      //
      // See step8_cpsr_alias_proof.md for the complete proof.
      //
      // Bit-by-bit justification (DDI0406C B1.3.3 vs DDI0403E B1.4.2):
      //   CPSR[31:27] N,Z,C,V,Q  = xPSR[31:27]  SAME
      //   CPSR[26:25] IT[1:0]    = xPSR[26:25]   SAME
      //   CPSR[19:16] GE          = xPSR[19:16]   SAME
      //   CPSR[15:10] IT[7:2]    = xPSR[15:10]   SAME
      //   CPSR[9]     E           = 0 (M-profile always LE)
      //   CPSR[5]     T           = PCState.thumb() (always 1)
      //   CPSR[4:0]   mode        = 0 (M-profile has no modes)
      case MISCREG_CPSR: {
          RegVal xpsr_val = miscRegs[MISCREG_M_XPSR];
          CPSR cpsr = 0;
          // Condition flags — same bit positions
          cpsr.nz  = bits(xpsr_val, 31, 30);
          cpsr.c   = bits(xpsr_val, 29);
          cpsr.v   = bits(xpsr_val, 28);
          cpsr.q   = bits(xpsr_val, 27);
          cpsr.ge  = bits(xpsr_val, 19, 16);
          // IT/ICI state — same bit positions
          cpsr.it1 = bits(xpsr_val, 26, 25);
          cpsr.it2 = bits(xpsr_val, 15, 10);
          // T bit: CPSR.t at bit[5], xPSR.T at bit[24].
          // Sync from PCState (same as A-profile isa.cc:464).
          auto pc = tc->pcState().as<PCState>();
          cpsr.t   = pc.thumb() ? 1 : 0;
          // E bit: M-profile always LE (DDI0403E A3.3.1).
          cpsr.e   = 0;
          // mode: M-profile has no mode bits.
          cpsr.mode = 0;

          DPRINTF(MProfileCPSR,
                  "CPSR read alias: xpsr=%#x, cpsr=%#x, PC=%#x\n",
                  xpsr_val, (RegVal)cpsr, tc->pcState().instAddr());
          return cpsr;
      }

      default:
          return miscRegs[idx];
    }
}

// =========================================================================
// setMiscRegNoEffect — raw array access (no side effects)
// =========================================================================

void
MISA::setMiscRegNoEffect(RegIndex idx, RegVal val)
{
    if (idx >= NUM_MISCREGS) {
        warn("MISA::setMiscRegNoEffect: index %d out of range", idx);
        return;
    }
    miscRegs[idx] = val;
}

// =========================================================================
// setMiscReg — with side effects, validation, and aliasing
// =========================================================================

void
MISA::setMiscReg(RegIndex idx, RegVal val)
{
    switch (idx) {
      case MISCREG_M_VTOR:
          // Enforce VTOR alignment: bits[N-1:0] are RES0.
          // DDI0403E B3.2.5.
          val &= vtorAlignMask;
          break;

      case MISCREG_M_CFSR:
          // Write-1-to-clear (W1C).  DDI0403E B3.2.15.
          val = miscRegs[MISCREG_M_CFSR] & ~val;
          break;

      case MISCREG_M_HFSR:
          // Write-1-to-clear.  DDI0403E B3.2.16.
          val = miscRegs[MISCREG_M_HFSR] & ~val;
          break;

      case MISCREG_M_DFSR:
          // Write-1-to-clear.  DDI0403E B3.2.17.
          val = miscRegs[MISCREG_M_DFSR] & ~val;
          break;

      case MISCREG_M_AIRCR: {
          // Writes require VECTKEY=0x05FA.  DDI0403E B3.2.6.
          AIRCR_t aircr = val;
          if (aircr.vectkey != 0x05FA)
              return;  // Wrong key — discard
          aircr.vectkey = 0xFA05;  // Readback value
          val = aircr;
          break;
      }

      case MISCREG_M_BASEPRI_MAX: {
          // Conditional-write: only commits when new value raises
          // priority ceiling (lower number = higher priority).
          // DDI0403E B5.2.5.
          RegVal cur = miscRegs[MISCREG_M_BASEPRI];
          if (val != 0 && (cur == 0 || val < cur))
              miscRegs[MISCREG_M_BASEPRI] = val;
          return;  // No fall-through — alias register
      }

      // =============================================================
      // CPSR → xPSR write aliasing
      // =============================================================

      case MISCREG_CPSR_Q: {
          // DSP saturation instructions write CpsrQ = (resTemp&1)<<27.
          // Q is a sticky flag — OR into xPSR.Q (never clear).
          // DDI0403E B1.4.2: Q can only be cleared by MSR.
          miscRegs[MISCREG_M_XPSR] |= (val & (1u << 27));
          DPRINTF(MProfileCPSR,
                  "CPSR_Q write alias: val=%#x, xpsr=%#x, PC=%#x\n",
                  val, miscRegs[MISCREG_M_XPSR],
                  tc->pcState().instAddr());
          return;
      }

      case MISCREG_CPSR: {
          // Full CPSR write — should NOT happen after MDecoder
          // intercepts all CPSR-writing instructions.
          // Safety net: extract valid fields and write to xPSR.
          DPRINTF(MProfileCPSR,
                  "WARNING: CPSR full write: val=%#x, PC=%#x\n",
                  val, tc->pcState().instAddr());
          RegVal xpsr = miscRegs[MISCREG_M_XPSR];
          xpsr = insertBits(xpsr, 31, 27, bits(val, 31, 27));
          xpsr = insertBits(xpsr, 19, 16, bits(val, 19, 16));
          xpsr = insertBits(xpsr, 26, 25, bits(val, 26, 25));
          xpsr = insertBits(xpsr, 15, 10, bits(val, 15, 10));
          miscRegs[MISCREG_M_XPSR] = xpsr;
          return;
      }

      // BUG-3 fix: CPUID is read-only (DDI0403E B3.2.3).
      // Silently discard writes.  On real hardware, writes to CPUID
      // are ignored.  Without this, a wild pointer or erroneous MMIO
      // write could corrupt the processor identification register.
      case MISCREG_M_CPUID:
          return;

      // BUG-6 fix: CONTROL.SPSEL write must swap R13 between MSP/PSP.
      // DDI0403E B1.4.4: writing CONTROL.SPSEL in Thread mode changes
      // which stack pointer R13 aliases to.  Without this, MSR CONTROL
      // with SPSEL=1 stores the new CONTROL value but R13 still points
      // to MSP, so Thread mode code using PSP gets the wrong stack.
      case MISCREG_M_CONTROL: {
          CONTROL_M oldCtrl = miscRegs[MISCREG_M_CONTROL];
          CONTROL_M newCtrl = val;

          // SPSEL swap is only meaningful in Thread mode (IPSR == 0).
          // In Handler mode, SPSEL is always effectively 0 (MSP).
          uint32_t ipsr = bits(miscRegs[MISCREG_M_XPSR], 8, 0);
          if (ipsr == 0 && oldCtrl.spsel != newCtrl.spsel) {
              // Save current R13 to the OLD stack's misc reg
              RegVal currentSP = tc->getReg(int_reg::Sp);
              if (oldCtrl.spsel == 0) {
                  miscRegs[MISCREG_M_MSP] = currentSP;
              } else {
                  miscRegs[MISCREG_M_PSP] = currentSP;
              }

              // Load R13 from the NEW stack's misc reg
              if (newCtrl.spsel == 0) {
                  tc->setReg(int_reg::Sp, miscRegs[MISCREG_M_MSP]);
              } else {
                  tc->setReg(int_reg::Sp, miscRegs[MISCREG_M_PSP]);
              }
          }

          miscRegs[MISCREG_M_CONTROL] = val;
          return;
      }

      default:
          break;
    }

    // Default: write to the array
    miscRegs[idx] = val;
}

// =========================================================================
// inUserMode — M-profile privilege check
// =========================================================================
//
// M-profile privilege is determined by CONTROL.nPRIV (bit 0):
//   0 = privileged (default in Thread mode)
//   1 = unprivileged
// Handler mode is always privileged regardless of CONTROL.nPRIV.
// DDI0403E B1.4.4.
//
// We check xPSR.IPSR: if nonzero, we're in Handler mode (privileged).
// If zero (Thread mode), check CONTROL.nPRIV.

bool
MISA::inUserMode() const
{
    // IPSR = xPSR[8:0] — exception number (0 = Thread mode)
    uint32_t ipsr = bits(miscRegs[MISCREG_M_XPSR], 8, 0);
    if (ipsr != 0)
        return false;  // Handler mode — always privileged

    // Thread mode: check CONTROL.nPRIV (bit 0)
    return bits(miscRegs[MISCREG_M_CONTROL], 0) == 1;
}

// =========================================================================
// copyRegsFrom — copy all state from another ThreadContext
// =========================================================================

void
MISA::copyRegsFrom(ThreadContext *src)
{
    // Integer registers (R0-R14 + PC via flat mapping)
    for (auto &id : flatIntRegClass)
        tc->setReg(id, src->getReg(id));

    // Condition code registers (NZ, C, V, GE, FP)
    for (auto &id : ccRegClass)
        tc->setReg(id, src->getReg(id));

    // Float registers (if FPU present)
    for (auto &id : floatRegClass)
        tc->setReg(id, src->getReg(id));

    // Misc registers — copy only the M-profile ones
    static const RegIndex mRegs[] = {
        MISCREG_M_XPSR, MISCREG_M_MSP, MISCREG_M_PSP,
        MISCREG_M_CONTROL, MISCREG_M_PRIMASK, MISCREG_M_BASEPRI,
        MISCREG_M_FAULTMASK, MISCREG_M_CPUID,
        // ICSR omitted — not a misc reg, computed by MProfileSCS
        MISCREG_M_VTOR, MISCREG_M_AIRCR, MISCREG_M_SCR,
        MISCREG_M_CCR, MISCREG_M_SHPR1, MISCREG_M_SHPR2,
        MISCREG_M_SHPR3, MISCREG_M_SHCSR, MISCREG_M_CFSR,
        MISCREG_M_HFSR, MISCREG_M_DFSR, MISCREG_M_MMFAR,
        MISCREG_M_BFAR, MISCREG_M_AFSR,
    };
    for (auto reg : mRegs)
        tc->setMiscRegNoEffect(reg, src->readMiscRegNoEffect(reg));

    // PC
    tc->pcState(src->pcState());
}

// =========================================================================
// Exclusive monitor for LDREX/STREX (DDI0403E A3.4.5)
// =========================================================================

void
MISA::handleLockedRead(const RequestPtr &req)
{
    lockedAddr = req->getPaddr();
}

bool
MISA::handleLockedWrite(const RequestPtr &req, Addr cacheBlockMask)
{
    // Compare at cache-line granularity, matching the A-profile ISA behavior.
    // LDREX marks the entire cache line; STREX to any address within the
    // same cache line should succeed.
    // DDI0403E A3.4.5: "The size of the marked block is
    // IMPLEMENTATION DEFINED, between one word and 2^10 words."
    if ((lockedAddr & cacheBlockMask) ==
        (req->getPaddr() & cacheBlockMask)) {
        // Same cache line — exclusive write succeeds
        lockedAddr = INVALID_LOCK_ADDR;
        return true;
    }
    // Different cache line — exclusive write fails.
    // Must set extraData so AtomicSimpleCPU::writeMem can read the
    // result via req->getExtraData() without hitting an assertion.
    // (A-profile's lockedWriteHandler does the same thing.)
    req->setExtraData(0);
    lockedAddr = INVALID_LOCK_ADDR;
    return false;
}

void
MISA::globalClearExclusive()
{
    lockedAddr = INVALID_LOCK_ADDR;
}

// =========================================================================
// Extension checking
// =========================================================================

bool
MISA::has(ArmExtension ext) const
{
    if (release)
        return release->has(ext);
    return false;
}

// =========================================================================
// Serialization
// =========================================================================

void
MISA::serialize(CheckpointOut &cp) const
{
    // BUG-5 fix: sync live CC flat regs → xPSR before checkpoint.
    // Without this, the checkpointed xPSR has stale NZCV/GE and
    // restoring from checkpoint would lose the current condition flags.
    if (tc)
        syncCCRegsToXpsr(tc);

    // Call base (saves isaName)
    BaseISA::serialize(cp);

    // Serialize only the M-profile misc regs
    SERIALIZE_ARRAY(miscRegs, NUM_MISCREGS);
    SERIALIZE_SCALAR(lockedAddr);
}

void
MISA::unserialize(CheckpointIn &cp)
{
    BaseISA::unserialize(cp);

    UNSERIALIZE_ARRAY(miscRegs, NUM_MISCREGS);
    UNSERIALIZE_SCALAR(lockedAddr);

    // BUG-5 fix: sync restored xPSR → CC flat regs so that execution
    // after checkpoint restore sees correct NZCV/GE condition flags.
    // tc may not be available yet during unserialize (gem5 restore
    // order: unserialize → startup).  If so, the sync will happen
    // when the first instruction executes and the flags are naturally
    // consumed.  But if tc is available, sync now for correctness.
    if (tc)
        syncXpsrToCCRegs(tc);
}

} // namespace ArmISA
} // namespace gem5
