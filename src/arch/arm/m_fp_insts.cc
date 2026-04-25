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
 * M-profile floating-point instruction implementations.
 *
 * Each doFpOp() reuses fplib math from vfp.hh — no computation
 * duplication.  The MFpOp::execute() wrapper handles M-profile FP
 * enable checking, CONTROL.FPCA, and lazy stacking for all children.
 */

#include "arch/arm/m_fp_insts.hh"

#include <cfenv>
#include <climits>
#include <cmath>
#include <limits>
#include <sstream>

#include "arch/arm/insts/fplib.hh"  // fplibMulAdd: bit-exact software FMA
#include "arch/arm/m_faults.hh"
#include "arch/arm/regs/int.hh"
#include "arch/arm/regs/misc.hh"
#include "arch/arm/regs/misc_types.hh"
#include "arch/arm/regs/vec.hh"
#include "arch/generic/memhelpers.hh"
#include "base/logging.hh"
#include "base/trace.hh"
#include "base/types.hh"
#include "cpu/exec_context.hh"
#include "cpu/thread_context.hh"
#include "debug/MProfileFP.hh"
#include "mem/packet_access.hh"

// =====================================================================
// Compile-time guards: verify host float is IEEE 754 single-precision.
//
// M-profile FP instructions rely on the host FPU producing bit-identical
// results to ARM FPv4-SP for basic operations (+, -, *, /, sqrt).
// IEEE 754 guarantees this for correctly-rounded operations when the
// host float format matches ARM's 32-bit single-precision.
// =====================================================================

static_assert(std::numeric_limits<float>::is_iec559,
    "M-profile FP requires IEEE 754 (IEC 559) compliant float");
static_assert(sizeof(float) == 4,
    "M-profile FP requires 32-bit float (ARM FPv4-SP single-precision)");
static_assert(std::numeric_limits<float>::digits == 24,
    "M-profile FP requires 24-bit float mantissa (IEEE 754 single)");

namespace gem5
{

namespace ArmISA
{

// M-profile BitUnion types (XPSR, CONTROL_M, CPACR, …) are declared
// inside namespace ArmMISA in misc_types.hh.  Same pattern as m_faults.cc.
using namespace ArmMISA;

// =====================================================================
// Runtime guard: verify host FP environment at first use.
//
// Checks that basic IEEE 754 operations produce the expected results
// and that rounding mode control works.  Called once; aborts
// simulation with a clear message if any check fails.
// =====================================================================

void
verifyHostFpEnvironment()
{
    // Verify rounding mode control works.
    int orig = fegetround();
    fesetround(FE_TOWARDZERO);
    volatile float a = 1.0f, b = 3.0f;
    volatile float result = a / b;
    fesetround(orig);

    // 1/3 rounded toward zero should be exactly 0x3EAAAAAA
    // (0.333333313..., truncated), not 0x3EAAAAAB (rounded nearest).
    uint32_t resultBits = floatToBits32(result);
    if (resultBits != 0x3EAAAAAA) {
        panic("M-profile FP: host fesetround(FE_TOWARDZERO) did not "
              "affect 1.0f/3.0f result.  Got %#x, expected 0x3EAAAAAA. "
              "Host FP environment is not suitable for ARM FPv4-SP "
              "simulation.", resultBits);
    }

    // Verify basic addition produces IEEE 754 correctly-rounded result.
    // 1.5f + 1.5f = 3.0f (exact, no rounding needed).
    volatile float x = 1.5f, y = 1.5f;
    volatile float sum = x + y;
    if (floatToBits32(sum) != 0x40400000) {  // 3.0f
        panic("M-profile FP: host float addition produced %#x, "
              "expected 0x40400000 (3.0f). "
              "Host FP is not suitable for ARM FPv4-SP simulation.",
              floatToBits32(sum));
    }

    // Verify rounding: 1.0f + 2^-24 should round to 1.0f in
    // round-to-nearest-even (the 2^-24 is exactly at the tie point
    // and rounds to even, keeping the LSB 0).
    volatile float tiny = bitsToFloat32(0x33800000);  // 2^-24
    volatile float one = 1.0f;
    volatile float rounded = one + tiny;
    if (floatToBits32(rounded) != 0x3F800000) {  // still 1.0f
        panic("M-profile FP: host float rounding is not IEEE 754 "
              "round-to-nearest-even. Got %#x for 1.0f + 2^-24.",
              floatToBits32(rounded));
    }
}

// =====================================================================
// M-profile FP computation helpers
// =====================================================================
//
// The host float type is IEEE 754 single-precision — identical to ARM
// FPv4-SP.  We use the host FPU for the computation and apply ARM-
// specific fixups for rounding mode, flush-to-zero, and default NaN.
//
// ARM VFP rounding mode → C standard rounding mode:
//   0 = RoundNearest  → FE_TONEAREST
//   1 = RoundUp       → FE_UPWARD
//   2 = RoundDown     → FE_DOWNWARD
//   3 = RoundZero     → FE_TOWARDZERO

static int
armRModeToHost(uint32_t rMode)
{
    switch (rMode) {
      case 0: return FE_TONEAREST;
      case 1: return FE_UPWARD;
      case 2: return FE_DOWNWARD;
      case 3: return FE_TOWARDZERO;
      default: return FE_TONEAREST;
    }
}

/** ARM default quiet NaN for single-precision. */
static constexpr uint32_t armDefaultNaN32 = 0x7FC00000;

/** Check if a float is denormal (subnormal). */
static inline bool
isDenormal(float val)
{
    uint32_t bits = floatToBits32(val);
    uint32_t exp = (bits >> 23) & 0xFF;
    uint32_t frac = bits & 0x7FFFFF;
    return (exp == 0) && (frac != 0);
}

float
mFpBinaryOp(FPSCR &fpscr, float op1, float op2,
            float (*func)(float, float))
{
    // 1. Flush denormal inputs to ±zero if FPSCR.FZ=1.
    //    Set IDC (Input Denormal Cumulative) flag.
    if (fpscr.fz) {
        if (isDenormal(op1)) {
            op1 = std::copysign(0.0f, op1);
            fpscr.idc = 1;
        }
        if (isDenormal(op2)) {
            op2 = std::copysign(0.0f, op2);
            fpscr.idc = 1;
        }
    }

    // 2. Set host rounding mode to match FPSCR.RMode.
    int savedRound = fegetround();
    fesetround(armRModeToHost(fpscr.rMode));

    // 3. Clear host FP exceptions and perform the operation.
    feclearexcept(FE_ALL_EXCEPT);
    float result = func(op1, op2);

    // 4. Capture host FP exceptions for FPSCR flag updates.
    int excepts = fetestexcept(FE_ALL_EXCEPT);
    if (excepts & FE_INVALID)
        fpscr.ioc = 1;
    if (excepts & FE_DIVBYZERO)
        fpscr.dzc = 1;
    if (excepts & FE_OVERFLOW)
        fpscr.ofc = 1;
    if (excepts & FE_UNDERFLOW)
        fpscr.ufc = 1;
    if (excepts & FE_INEXACT)
        fpscr.ixc = 1;

    // 5. Restore host rounding mode.
    fesetround(savedRound);

    // 6. Handle NaN result per ARM rules.
    if (std::isnan(result)) {
        bool nan1 = std::isnan(op1);
        bool nan2 = std::isnan(op2);
        bool sig1 = nan1 && !(floatToBits32(op1) & (1u << 22));
        bool sig2 = nan2 && !(floatToBits32(op2) & (1u << 22));

        // Signaling NaN → raise Invalid Operation.
        if (sig1 || sig2)
            fpscr.ioc = 1;

        if (fpscr.dn || (!nan1 && !nan2)) {
            // Default NaN mode, or result is NaN from non-NaN inputs
            // (e.g., 0/0, inf-inf): return ARM default NaN.
            result = bitsToFloat32(armDefaultNaN32);
        } else if (sig1) {
            // Quieten signaling NaN from op1.
            result = bitsToFloat32(floatToBits32(op1) | (1u << 22));
        } else if (sig2) {
            result = bitsToFloat32(floatToBits32(op2) | (1u << 22));
        } else if (nan1) {
            result = op1;  // Propagate quiet NaN from op1.
        } else {
            result = op2;
        }
    }

    // 7. Flush denormal output to ±zero if FPSCR.FZ=1.
    if (fpscr.fz && isDenormal(result)) {
        result = std::copysign(0.0f, result);
        fpscr.ufc = 1;
    }

    return result;
}

float
mFpUnaryOp(FPSCR &fpscr, float op1, float (*func)(float))
{
    // 1. Flush denormal input if FPSCR.FZ=1.
    if (fpscr.fz && isDenormal(op1)) {
        op1 = std::copysign(0.0f, op1);
        fpscr.idc = 1;
    }

    // 2. Set host rounding mode.
    int savedRound = fegetround();
    fesetround(armRModeToHost(fpscr.rMode));

    // 3. Compute.
    feclearexcept(FE_ALL_EXCEPT);
    float result = func(op1);

    // 4. Capture exceptions.
    int excepts = fetestexcept(FE_ALL_EXCEPT);
    if (excepts & FE_INVALID)   fpscr.ioc = 1;
    if (excepts & FE_OVERFLOW)  fpscr.ofc = 1;
    if (excepts & FE_UNDERFLOW) fpscr.ufc = 1;
    if (excepts & FE_INEXACT)   fpscr.ixc = 1;

    // 5. Restore rounding mode.
    fesetround(savedRound);

    // 6. NaN fixup.
    if (std::isnan(result)) {
        bool nan1 = std::isnan(op1);
        bool sig1 = nan1 && !(floatToBits32(op1) & (1u << 22));
        if (sig1) fpscr.ioc = 1;

        if (fpscr.dn || !nan1) {
            result = bitsToFloat32(armDefaultNaN32);
        } else if (sig1) {
            result = bitsToFloat32(floatToBits32(op1) | (1u << 22));
        } else {
            result = op1;
        }
    }

    // 7. Flush denormal output.
    if (fpscr.fz && isDenormal(result)) {
        result = std::copysign(0.0f, result);
        fpscr.ufc = 1;
    }

    return result;
}

// =====================================================================
// MFpOp — Common wrapper methods
// =====================================================================

Fault
MFpOp::checkMProfileFPEnabled(ThreadContext *tc) const
{
    // CPACR.CP10 (bits [21:20]) must be 0b11 for full FP access.
    // 0b01 = privileged only, 0b00 = denied, 0b10 = reserved.
    // [DDI0403E B3.2.20]
    CPACR cpacr = tc->readMiscRegNoEffect(MISCREG_M_CPACR);
    uint8_t cp10 = cpacr.cp10;

    if (cp10 == 0x3) {
        // Full access — always allowed.
        return NoFault;
    }

    if (cp10 == 0x1) {
        // Privileged access only.  Check if we're privileged.
        // Handler mode (IPSR != 0) is always privileged.
        // Thread mode: check CONTROL.nPRIV.
        XPSR xpsr = tc->readMiscRegNoEffect(MISCREG_M_XPSR);
        bool inHandler = (xpsr.exception != 0);
        if (inHandler)
            return NoFault;

        CONTROL_M ctrl = tc->readMiscRegNoEffect(MISCREG_M_CONTROL);
        if (ctrl.npriv == 0)
            return NoFault;  // Privileged thread mode
    }

    // Access denied — UsageFault with NOCP indication.
    // TODO: set CFSR.UFSR.NOCP bit when CFSR bitfield support is added.
    DPRINTF(MProfileFP, "FP access denied: CPACR.CP10=%d, UsageFault\n",
            cp10);
    return std::make_shared<ArmMFault>(MPEXC_USAGEFAULT);
}

void
MFpOp::setFPCA(ThreadContext *tc) const
{
    // Set CONTROL.FPCA (bit 2) = 1 to indicate FP context is active.
    // This tells exception entry to stack the extended FP frame.
    // [DDI0403E B1.4.2]: "the processor sets this bit to 1 on
    // execution of a floating-point instruction."
    CONTROL_M ctrl = tc->readMiscRegNoEffect(MISCREG_M_CONTROL);
    if (!ctrl.fpca) {
        ctrl.fpca = 1;
        tc->setMiscRegNoEffect(MISCREG_M_CONTROL, ctrl);
    }
}

Fault
MFpOp::completeLazyStacking(ThreadContext *tc) const
{
    // Stub: lazy stacking not yet implemented.
    // When FPCCR.LSPEN=1 and FPCCR.LSPACT=1, the hardware deferred
    // saving FP registers during exception entry.  The first FP
    // instruction in the handler must complete that deferred save.
    // [DDI0403E B1.5.7]
    //
    // Full implementation requires:
    //   1. Check FPCCR.LSPACT — if 0, nothing to do.
    //   2. Read FPCAR for the deferred save address.
    //   3. Push S0-S15 + FPSCR to that address.
    //   4. Clear FPCCR.LSPACT.
    return NoFault;
}

Fault
MFpOp::execute(ExecContext *xc, trace::InstRecord *traceData) const
{
    ThreadContext *tc = xc->tcBase();

    DPRINTF(MProfileFP, "MFpOp::execute: %s pc=%#x\n",
            mnemonic,
            tc->pcState().as<ArmISA::PCState>().instAddr());

    // 1. Check FP access permission (CPACR.CP10)
    Fault fault = checkMProfileFPEnabled(tc);
    if (fault != NoFault)
        return fault;

    // 2. Set CONTROL.FPCA = 1 (FP context now active)
    setFPCA(tc);

    // 3. Complete lazy stacking if pending
    fault = completeLazyStacking(tc);
    if (fault != NoFault)
        return fault;

    // 4. Dispatch to child's actual FP computation
    return doFpOp(xc, traceData);
}

// =====================================================================
// MFpBinS — Binary single-precision operations (VADD, VSUB, VMUL, etc.)
// =====================================================================

Fault
MFpBinS::doFpOp(ExecContext *xc, trace::InstRecord *traceData) const
{
    ThreadContext *tc = xc->tcBase();

    FPSCR fpscr = tc->readMiscRegNoEffect(MISCREG_FPSCR);

    float val1 = bitsToFloat32(tc->getReg(vfpSRegId(op1)));
    float val2 = bitsToFloat32(tc->getReg(vfpSRegId(op2)));

    float result = mFpBinaryOp(fpscr, val1, val2, func);

    uint32_t resultBits = floatToBits32(result);
    tc->setReg(vfpSRegId(dest), (RegVal)resultBits);
    tc->setMiscRegNoEffect(MISCREG_FPSCR, fpscr);

    DPRINTF(MProfileFP, "%s s%d, s%d, s%d: %g %s %g = %g [%#x]\n",
            mnemonic, dest, op1, op2, val1, opName, val2, result,
            resultBits);

    if (traceData)
        traceData->setData(vecElemClass, (RegVal)resultBits);

    return NoFault;
}

std::string
MFpBinS::generateDisassembly(
    Addr pc, const loader::SymbolTable *symtab) const
{
    std::ostringstream ss;
    ss << mnemonic << " s" << dest << ", s" << op1 << ", s" << op2;
    return ss.str();
}

// =====================================================================
// MFpTernaryS — Ternary single-precision (VFMA, VFMS, VFNMA, VFNMS)
// =====================================================================

Fault
MFpTernaryS::doFpOp(ExecContext *xc,
                     trace::InstRecord *traceData) const
{
    ThreadContext *tc = xc->tcBase();
    FPSCR fpscr = tc->readMiscRegNoEffect(MISCREG_FPSCR);

    float acc = bitsToFloat32(tc->getReg(vfpSRegId(dest)));
    float val1 = bitsToFloat32(tc->getReg(vfpSRegId(op1)));
    float val2 = bitsToFloat32(tc->getReg(vfpSRegId(op2)));

    // Flush denormals if FPSCR.FZ=1
    if (fpscr.fz) {
        if (isDenormal(acc)) {
            acc = std::copysign(0.0f, acc);
            fpscr.idc = 1;
        }
        if (isDenormal(val1)) {
            val1 = std::copysign(0.0f, val1);
            fpscr.idc = 1;
        }
        if (isDenormal(val2)) {
            val2 = std::copysign(0.0f, val2);
            fpscr.idc = 1;
        }
    }

    // Set host rounding mode
    int savedRound = fegetround();
    fesetround(armRModeToHost(fpscr.rMode));

    feclearexcept(FE_ALL_EXCEPT);
    float result = func(acc, val1, val2);

    // Capture host FP exceptions
    int excepts = fetestexcept(FE_ALL_EXCEPT);
    if (excepts & FE_INVALID)   fpscr.ioc = 1;
    if (excepts & FE_OVERFLOW)  fpscr.ofc = 1;
    if (excepts & FE_UNDERFLOW) fpscr.ufc = 1;
    if (excepts & FE_INEXACT)   fpscr.ixc = 1;

    fesetround(savedRound);

    // NaN fixup
    if (std::isnan(result)) {
        bool nanA = std::isnan(acc);
        bool nan1 = std::isnan(val1);
        bool nan2 = std::isnan(val2);
        bool sigA = nanA && !(floatToBits32(acc) & (1u << 22));
        bool sig1 = nan1 && !(floatToBits32(val1) & (1u << 22));
        bool sig2 = nan2 && !(floatToBits32(val2) & (1u << 22));

        if (sigA || sig1 || sig2)
            fpscr.ioc = 1;

        if (fpscr.dn || (!nanA && !nan1 && !nan2)) {
            result = bitsToFloat32(armDefaultNaN32);
        } else if (sigA) {
            result = bitsToFloat32(floatToBits32(acc) | (1u << 22));
        } else if (sig1) {
            result = bitsToFloat32(floatToBits32(val1) | (1u << 22));
        } else if (sig2) {
            result = bitsToFloat32(floatToBits32(val2) | (1u << 22));
        } else if (nanA) {
            result = acc;
        } else if (nan1) {
            result = val1;
        } else {
            result = val2;
        }
    }

    // Flush denormal output
    if (fpscr.fz && isDenormal(result)) {
        result = std::copysign(0.0f, result);
        fpscr.ufc = 1;
    }

    uint32_t resultBits = floatToBits32(result);
    tc->setReg(vfpSRegId(dest), (RegVal)resultBits);
    tc->setMiscRegNoEffect(MISCREG_FPSCR, fpscr);

    if (traceData)
        traceData->setData(vecElemClass, (RegVal)resultBits);

    return NoFault;
}

std::string
MFpTernaryS::generateDisassembly(
    Addr pc, const loader::SymbolTable *symtab) const
{
    std::ostringstream ss;
    ss << mnemonic << " s" << dest << ", s" << op1 << ", s" << op2;
    return ss.str();
}

// =====================================================================
// MFpFusedMulAddS — Fused ternary single-precision
// (VFMA, VFMS, VFNMA, VFNMS)
//
// Delegates to `fplibMulAdd<uint32_t>` for a SINGLE-ROUND fused
// multiply-add on bit patterns. This differs from MFpTernaryS in two
// crucial ways that matter for VFMA correctness and cross-host
// determinism:
//
//   1. fplibMulAdd rounds once (correct per ARM VFMA spec); the host
//      `a + n*m` lambda used by MFpTernaryS rounds twice (wrong).
//   2. fplibMulAdd does not touch host FP state — no fesetround, no
//      fetestexcept. It reads/writes IEEE 754 bits directly and
//      updates FPSCR.IOC/OFC/UFC/IXC/IDC via set_fpscr0 inside
//      fplib.cc. Results are byte-identical on any host.
//
// VMLA/VMLS/VNMLA/VNMLS are NOT fused (ARM spec: `FPAdd(FPRegD[d],
// FPMul(...), …)` = two roundings), so they stay on MFpTernaryS with
// its `a + n*m`-style lambda.
// =====================================================================

Fault
MFpFusedMulAddS::doFpOp(ExecContext *xc,
                        trace::InstRecord *traceData) const
{
    ThreadContext *tc = xc->tcBase();
    FPSCR fpscr = tc->readMiscRegNoEffect(MISCREG_FPSCR);

    // Operate on 32-bit IEEE 754 bit patterns. The S-reg register file
    // stores values as RegVal (64-bit); we truncate to the low 32 bits.
    uint32_t addend = (uint32_t)tc->getReg(vfpSRegId(dest));
    uint32_t p1     = (uint32_t)tc->getReg(vfpSRegId(op1));
    uint32_t p2     = (uint32_t)tc->getReg(vfpSRegId(op2));

    // Capture the ORIGINAL register bits (pre-negation) for tracing.
    // Lets the MProfileFP debug log show what the architected registers
    // held going into this fused mul-add, independent of the variant-
    // specific sign flips done below.
    const uint32_t orig_addend = addend;
    const uint32_t orig_p1     = p1;

    // FPNeg per ARM ARM is bit-31 flip only; payload is preserved for
    // NaNs. fplibMulAdd's internal NaN processing sees the flipped
    // sign and propagates correctly.
    if (negateAddend)  addend ^= 0x80000000u;
    if (negateProduct) p1     ^= 0x80000000u;

    // M-profile has no FPCR; default-constructed FPCR (all bits 0)
    // gives the FPv4-SP behavior fplibMulAdd expects (no AH/FIZ/NEP).
    // FPSCR is updated in place with sticky IOC/OFC/UFC/IXC/IDC flags.
    FPCR fpcr = 0;
    uint32_t result = fplibMulAdd<uint32_t>(addend, p1, p2, fpscr, fpcr);

    tc->setReg(vfpSRegId(dest), (RegVal)result);
    tc->setMiscRegNoEffect(MISCREG_FPSCR, fpscr);

    // Diagnostic trace: architected inputs (pre-negation), variant
    // flags, and architected output — all as hex bits and decoded
    // floats. Primarily used to bisect numerical drift in benchmarks
    // like tinympc where accumulators blow up over many VFMAs.
    DPRINTF(MProfileFP,
            "%s s%d, s%d, s%d: sd=%g[0x%08x] sn=%g[0x%08x] "
            "sm=%g[0x%08x] [negA=%d negP=%d] = %g[0x%08x]\n",
            mnemonic, dest, op1, op2,
            bitsToFloat32(orig_addend), orig_addend,
            bitsToFloat32(orig_p1), orig_p1,
            bitsToFloat32(p2), p2,
            (int)negateAddend, (int)negateProduct,
            bitsToFloat32(result), result);

    if (traceData)
        traceData->setData(vecElemClass, (RegVal)result);

    return NoFault;
}

std::string
MFpFusedMulAddS::generateDisassembly(
    Addr pc, const loader::SymbolTable *symtab) const
{
    std::ostringstream ss;
    ss << mnemonic << " s" << dest << ", s" << op1 << ", s" << op2;
    return ss.str();
}

// =====================================================================
// MFpUnaryS — Unary single-precision operations (VNEG, VABS)
// =====================================================================

Fault
MFpUnaryS::doFpOp(ExecContext *xc, trace::InstRecord *traceData) const
{
    ThreadContext *tc = xc->tcBase();

    FPSCR fpscr = tc->readMiscRegNoEffect(MISCREG_FPSCR);
    float val = bitsToFloat32(tc->getReg(vfpSRegId(op1)));

    float result = mFpUnaryOp(fpscr, val, func);

    uint32_t resultBits = floatToBits32(result);
    tc->setReg(vfpSRegId(dest), (RegVal)resultBits);
    tc->setMiscRegNoEffect(MISCREG_FPSCR, fpscr);

    if (traceData)
        traceData->setData(vecElemClass, (RegVal)resultBits);

    return NoFault;
}

std::string
MFpUnaryS::generateDisassembly(
    Addr pc, const loader::SymbolTable *symtab) const
{
    std::ostringstream ss;
    ss << mnemonic << " s" << dest << ", s" << op1;
    return ss.str();
}

// =====================================================================
// MFpMovImmS — VMOV.F32 Sd, #imm
// =====================================================================

Fault
MFpMovImmS::doFpOp(ExecContext *xc, trace::InstRecord *traceData) const
{
    ThreadContext *tc = xc->tcBase();

    // imm is already the expanded 32-bit float value
    tc->setReg(vfpSRegId(dest), (RegVal)imm);

    DPRINTF(MProfileFP, "vmov.f32 s%d, #%g [%#x]\n",
            dest, bitsToFloat32(imm), imm);

    if (traceData)
        traceData->setData(vecElemClass, (RegVal)imm);

    return NoFault;
}

std::string
MFpMovImmS::generateDisassembly(
    Addr pc, const loader::SymbolTable *symtab) const
{
    std::ostringstream ss;
    ss << "vmov.f32 s" << dest << ", #" << bitsToFloat32(imm);
    return ss.str();
}

// =====================================================================
// MFpMovRegS — VMOV.F32 Sd, Sm
// =====================================================================

Fault
MFpMovRegS::doFpOp(ExecContext *xc, trace::InstRecord *traceData) const
{
    ThreadContext *tc = xc->tcBase();

    RegVal val = tc->getReg(vfpSRegId(op1));
    tc->setReg(vfpSRegId(dest), val);

    if (traceData)
        traceData->setData(vecElemClass, val);

    return NoFault;
}

std::string
MFpMovRegS::generateDisassembly(
    Addr pc, const loader::SymbolTable *symtab) const
{
    std::ostringstream ss;
    ss << "vmov.f32 s" << dest << ", s" << op1;
    return ss.str();
}

// =====================================================================
// MFpMovCoreToS — VMOV Sn, Rd (core register → VFP)
// =====================================================================

Fault
MFpMovCoreToS::doFpOp(ExecContext *xc, trace::InstRecord *traceData) const
{
    ThreadContext *tc = xc->tcBase();

    // Read core register (32-bit)
    RegVal val = tc->getReg(RegId(intRegClass, rt)) & 0xFFFFFFFF;
    tc->setReg(vfpSRegId(sd), val);

    if (traceData)
        traceData->setData(vecElemClass, val);

    return NoFault;
}

std::string
MFpMovCoreToS::generateDisassembly(
    Addr pc, const loader::SymbolTable *symtab) const
{
    std::ostringstream ss;
    ss << "vmov s" << sd << ", r" << rt;
    return ss.str();
}

// =====================================================================
// MFpMovSToCore — VMOV Rd, Sn (VFP → core register)
// =====================================================================

Fault
MFpMovSToCore::doFpOp(ExecContext *xc, trace::InstRecord *traceData) const
{
    ThreadContext *tc = xc->tcBase();

    RegVal val = tc->getReg(vfpSRegId(sn)) & 0xFFFFFFFF;
    tc->setReg(RegId(intRegClass, rt), val);

    if (traceData)
        traceData->setData(intRegClass, val);

    return NoFault;
}

std::string
MFpMovSToCore::generateDisassembly(
    Addr pc, const loader::SymbolTable *symtab) const
{
    std::ostringstream ss;
    ss << "vmov r" << rt << ", s" << sn;
    return ss.str();
}

// =====================================================================
// MFpMovCorePairToD — VMOV Dm, Rt, Rt2
// =====================================================================

Fault
MFpMovCorePairToD::doFpOp(ExecContext *xc,
                           trace::InstRecord *traceData) const
{
    // Rt → S(dd*2) (low word), Rt2 → S(dd*2+1) (high word)
    uint32_t lo = (uint32_t)xc->getRegOperand(this, 0);
    uint32_t hi = (uint32_t)xc->getRegOperand(this, 1);
    xc->setRegOperand(this, 0, (RegVal)lo);
    xc->setRegOperand(this, 1, (RegVal)hi);
    if (traceData) {
        uint64_t val = ((uint64_t)hi << 32) | lo;
        traceData->setData(val);
    }
    return NoFault;
}

std::string
MFpMovCorePairToD::generateDisassembly(
    Addr pc, const loader::SymbolTable *symtab) const
{
    std::ostringstream ss;
    ss << "vmov d" << dd << ", r" << rt << ", r" << rt2;
    return ss.str();
}

// =====================================================================
// MFpMovDToCorePair — VMOV Rt, Rt2, Dm
// =====================================================================

Fault
MFpMovDToCorePair::doFpOp(ExecContext *xc,
                           trace::InstRecord *traceData) const
{
    // S(dd*2) → Rt (low word), S(dd*2+1) → Rt2 (high word)
    uint32_t lo = (uint32_t)xc->getRegOperand(this, 0);
    uint32_t hi = (uint32_t)xc->getRegOperand(this, 1);
    xc->setRegOperand(this, 0, (RegVal)lo);
    xc->setRegOperand(this, 1, (RegVal)hi);
    if (traceData) {
        uint64_t val = ((uint64_t)hi << 32) | lo;
        traceData->setData(val);
    }
    return NoFault;
}

std::string
MFpMovDToCorePair::generateDisassembly(
    Addr pc, const loader::SymbolTable *symtab) const
{
    std::ostringstream ss;
    ss << "vmov r" << rt << ", r" << rt2 << ", d" << dd;
    return ss.str();
}

// =====================================================================
// MFpCmpS — VCMP.F32 / VCMPE.F32
// =====================================================================

Fault
MFpCvtS::doFpOp(ExecContext *xc, trace::InstRecord *traceData) const
{
    ThreadContext *tc = xc->tcBase();
    FPSCR fpscr = tc->readMiscRegNoEffect(MISCREG_FPSCR);

    uint32_t srcBits = (uint32_t)tc->getReg(vfpSRegId(op1));
    uint32_t resultBits;

    if (toFloat) {
        // Integer → float: use round-to-nearest (ARM spec, not RMode)
        int savedRound = fegetround();
        fesetround(FE_TONEAREST);
        feclearexcept(FE_ALL_EXCEPT);

        float result;
        if (isSigned) {
            int32_t intVal;
            std::memcpy(&intVal, &srcBits, 4);
            result = (float)intVal;
        } else {
            result = (float)srcBits;
        }

        int excepts = fetestexcept(FE_ALL_EXCEPT);
        if (excepts & FE_INEXACT) fpscr.ixc = 1;

        fesetround(savedRound);
        resultBits = floatToBits32(result);
    } else {
        // Float → integer: use round-towards-zero (ARM spec)
        float srcFloat = bitsToFloat32(srcBits);

        // Flush denormal input if FPSCR.FZ
        if (fpscr.fz && isDenormal(srcFloat)) {
            srcFloat = std::copysign(0.0f, srcFloat);
            fpscr.idc = 1;
        }

        int savedRound = fegetround();
        fesetround(FE_TOWARDZERO);
        feclearexcept(FE_ALL_EXCEPT);

        if (isSigned) {
            int32_t intResult;
            if (std::isnan(srcFloat)) {
                intResult = 0;
                fpscr.ioc = 1;
            } else if (srcFloat >= 2147483648.0f) {
                intResult = 2147483647;
                fpscr.ioc = 1;
            } else if (srcFloat < -2147483648.0f) {
                intResult = -2147483648;
                fpscr.ioc = 1;
            } else {
                intResult = (int32_t)srcFloat;
            }
            std::memcpy(&resultBits, &intResult, 4);
        } else {
            uint32_t uintResult;
            if (std::isnan(srcFloat) || srcFloat < 0.0f) {
                uintResult = 0;
                fpscr.ioc = 1;
            } else if (srcFloat >= 4294967296.0f) {
                uintResult = 0xFFFFFFFF;
                fpscr.ioc = 1;
            } else {
                uintResult = (uint32_t)srcFloat;
            }
            resultBits = uintResult;
        }

        int excepts = fetestexcept(FE_ALL_EXCEPT);
        if (excepts & FE_INEXACT) fpscr.ixc = 1;
        fesetround(savedRound);
    }

    tc->setReg(vfpSRegId(dest), (RegVal)resultBits);
    tc->setMiscRegNoEffect(MISCREG_FPSCR, fpscr);

    if (traceData)
        traceData->setData(vecElemClass, (RegVal)resultBits);

    return NoFault;
}

std::string
MFpCvtS::generateDisassembly(Addr pc,
    const loader::SymbolTable *symtab) const
{
    std::ostringstream ss;
    if (toFloat) {
        ss << "vcvt.f32." << (isSigned ? "s32" : "u32");
    } else {
        ss << "vcvt." << (isSigned ? "s32" : "u32") << ".f32";
    }
    ss << " s" << dest << ", s" << op1;
    return ss.str();
}

// =====================================================================
// MFpCmpS — VCMP.F32 / VCMPE.F32
// =====================================================================

Fault
MFpCmpS::doFpOp(ExecContext *xc, trace::InstRecord *traceData) const
{
    ThreadContext *tc = xc->tcBase();

    FPSCR fpscr = tc->readMiscRegNoEffect(MISCREG_FPSCR);
    float val1 = bitsToFloat32(tc->getReg(vfpSRegId(op1)));
    float val2 = withZero ? 0.0f
                          : bitsToFloat32(tc->getReg(vfpSRegId(op2)));

    // Flush denormal inputs to zero if FPSCR.FZ is set.
    if (fpscr.fz) {
        if (isDenormal(val1)) {
            val1 = std::copysign(0.0f, val1);
            fpscr.idc = 1;
        }
        if (isDenormal(val2)) {
            val2 = std::copysign(0.0f, val2);
            fpscr.idc = 1;
        }
    }

    // Compare and set FPSCR.NZCV (DDI0403E A7.5.1)
    if (val1 == val2) {
        fpscr.n = 0; fpscr.z = 1; fpscr.c = 1; fpscr.v = 0;
    } else if (val1 < val2) {
        fpscr.n = 1; fpscr.z = 0; fpscr.c = 0; fpscr.v = 0;
    } else if (val1 > val2) {
        fpscr.n = 0; fpscr.z = 0; fpscr.c = 1; fpscr.v = 0;
    } else {
        // Unordered (one or both NaN)
        fpscr.n = 0; fpscr.z = 0; fpscr.c = 1; fpscr.v = 1;
        if (withExc) {
            // VCMPE: raise Invalid Operation for any NaN
            fpscr.ioc = 1;
        }
    }

    tc->setMiscRegNoEffect(MISCREG_FPSCR, fpscr);

    DPRINTF(MProfileFP, "vcmp%s.f32 s%d(%g), %s: NZCV=%d%d%d%d\n",
            withExc ? "e" : "", op1, val1,
            withZero ? "#0.0" : std::to_string(val2).c_str(),
            (int)fpscr.n, (int)fpscr.z, (int)fpscr.c, (int)fpscr.v);

    return NoFault;
}

std::string
MFpCmpS::generateDisassembly(
    Addr pc, const loader::SymbolTable *symtab) const
{
    std::ostringstream ss;
    ss << (withExc ? "vcmpe.f32 " : "vcmp.f32 ");
    ss << "s" << op1;
    if (withZero)
        ss << ", #0.0";
    else
        ss << ", s" << op2;
    return ss.str();
}

// =====================================================================
// MFpMrs — VMRS Rd, FPSCR
// =====================================================================

Fault
MFpMrs::doFpOp(ExecContext *xc, trace::InstRecord *traceData) const
{
    ThreadContext *tc = xc->tcBase();

    RegVal fpscr = tc->readMiscRegNoEffect(MISCREG_FPSCR);

    if (rt == 0xF) {
        // VMRS APSR_nzcv, FPSCR — copy FPSCR.NZCV to APSR flags.
        // On M-profile, APSR flags are in CC flat regs (not xPSR directly).
        RegVal nzcv = (fpscr >> 28) & 0xF;
        // NZ is packed as {N, Z} in cc_reg::Nz
        tc->setReg(cc_reg::Nz, (RegVal)(((nzcv >> 3) & 1) << 1 |
                                         ((nzcv >> 2) & 1)));
        tc->setReg(cc_reg::C, (RegVal)((nzcv >> 1) & 1));
        tc->setReg(cc_reg::V, (RegVal)(nzcv & 1));
        DPRINTF(MProfileFP, "vmrs APSR_nzcv, FPSCR: NZCV=%x [FPSCR=%#x]\n",
                nzcv, fpscr);
    } else {
        tc->setReg(RegId(intRegClass, rt), fpscr);
        DPRINTF(MProfileFP, "vmrs r%d, FPSCR: %#x\n", rt, fpscr);
    }

    if (traceData)
        traceData->setData(intRegClass, fpscr);

    return NoFault;
}

std::string
MFpMrs::generateDisassembly(
    Addr pc, const loader::SymbolTable *symtab) const
{
    std::ostringstream ss;
    ss << "vmrs ";
    if (rt == 0xF)
        ss << "APSR_nzcv";
    else
        ss << "r" << rt;
    ss << ", fpscr";
    return ss.str();
}

// =====================================================================
// MFpMsr — VMSR FPSCR, Rd
// =====================================================================

Fault
MFpMsr::doFpOp(ExecContext *xc, trace::InstRecord *traceData) const
{
    ThreadContext *tc = xc->tcBase();

    RegVal val = tc->getReg(RegId(intRegClass, rt));
    tc->setMiscRegNoEffect(MISCREG_FPSCR, val);

    DPRINTF(MProfileFP, "vmsr FPSCR, r%d: %#x\n", rt, val);

    if (traceData)
        traceData->setData(intRegClass, val);

    return NoFault;
}

std::string
MFpMsr::generateDisassembly(
    Addr pc, const loader::SymbolTable *symtab) const
{
    std::ostringstream ss;
    ss << "vmsr fpscr, r" << rt;
    return ss.str();
}

// =====================================================================
// MFpLdrS — VLDR.32 Sd, [Rn, #imm]
// =====================================================================

Fault
MFpLdrS::execute(ExecContext *xc, trace::InstRecord *traceData) const
{
    ThreadContext *tc = xc->tcBase();
    Fault fault = checkMProfileFPEnabled(tc);
    if (fault != NoFault) return fault;
    setFPCA(tc);

    // PC-relative base for VLDR (literal). Thumb semantics (ARM ARM
    // A6.1.2): the architected PC inside any instruction is
    // `instruction_address + 4`, and VLDR (literal) then Align()s it
    // to 4. `instAddr()` returns the raw instruction address without
    // the +4 pipeline offset, so we add 4 explicitly before aligning.
    // Skipping the +4 pulled the wrong word from the literal pool
    // (e.g. Eigen's gebp zero-accumulator load was reading garbage,
    // poisoning every VFMA — see tinympc iter1 blowup trace).
    Addr base = (rn == int_reg::Pc)
        ? ((tc->pcState().as<ArmISA::PCState>().instAddr() + 4) & ~0x3)
        : tc->getReg(RegId(intRegClass, rn));
    Addr addr = add ? (base + imm) : (base - imm);

    uint32_t data = 0;
    fault = gem5::readMemAtomicLE(xc, traceData, addr, data,
                                  Request::Flags(0));
    if (fault != NoFault) return fault;
    xc->setRegOperand(this, 0, (RegVal)data);
    return NoFault;
}

Fault
MFpLdrS::initiateAcc(ExecContext *xc, trace::InstRecord *traceData) const
{
    ThreadContext *tc = xc->tcBase();
    Fault fault = checkMProfileFPEnabled(tc);
    if (fault != NoFault) return fault;
    setFPCA(tc);

    // PC-relative base for VLDR (literal). Thumb semantics (ARM ARM
    // A6.1.2): the architected PC inside any instruction is
    // `instruction_address + 4`, and VLDR (literal) then Align()s it
    // to 4. `instAddr()` returns the raw instruction address without
    // the +4 pipeline offset, so we add 4 explicitly before aligning.
    // Skipping the +4 pulled the wrong word from the literal pool
    // (e.g. Eigen's gebp zero-accumulator load was reading garbage,
    // poisoning every VFMA — see tinympc iter1 blowup trace).
    Addr base = (rn == int_reg::Pc)
        ? ((tc->pcState().as<ArmISA::PCState>().instAddr() + 4) & ~0x3)
        : tc->getReg(RegId(intRegClass, rn));
    Addr addr = add ? (base + imm) : (base - imm);

    DPRINTF(MProfileFP, "MFpLdrS::initiateAcc: s%d [r%d%s%d] "
            "rn_is_pc=%d base=%#x imm=%d add=%d addr=%#x\n",
            sd, rn, add ? "+" : "-", imm,
            (rn == int_reg::Pc), base, imm, add, addr);

    uint32_t dummy = 0;
    return gem5::initiateMemRead(xc, traceData, addr, dummy,
                                 Request::Flags(0));
}

Fault
MFpLdrS::completeAcc(PacketPtr pkt, ExecContext *xc,
                      trace::InstRecord *traceData) const
{
    uint32_t data = 0;
    gem5::getMemLE(pkt, data, traceData);
    xc->setRegOperand(this, 0, (RegVal)data);
    return NoFault;
}

std::string
MFpLdrS::generateDisassembly(Addr pc,
    const loader::SymbolTable *symtab) const
{
    std::ostringstream ss;
    ss << "vldr s" << sd << ", [r" << rn
       << ", #" << (add ? "+" : "-") << imm << "]";
    return ss.str();
}

// =====================================================================
// MFpStrS — VSTR.32 Sd, [Rn, #imm]
// =====================================================================

Fault
MFpStrS::execute(ExecContext *xc, trace::InstRecord *traceData) const
{
    ThreadContext *tc = xc->tcBase();
    Fault fault = checkMProfileFPEnabled(tc);
    if (fault != NoFault) return fault;
    setFPCA(tc);

    Addr base = tc->getReg(RegId(intRegClass, rn));
    Addr addr = add ? (base + imm) : (base - imm);

    uint32_t data = (uint32_t)xc->getRegOperand(this, 1);
    return gem5::writeMemAtomicLE(xc, traceData, data, addr,
                                  Request::Flags(0), nullptr);
}

Fault
MFpStrS::initiateAcc(ExecContext *xc, trace::InstRecord *traceData) const
{
    ThreadContext *tc = xc->tcBase();
    Fault fault = checkMProfileFPEnabled(tc);
    if (fault != NoFault) return fault;
    setFPCA(tc);

    Addr base = tc->getReg(RegId(intRegClass, rn));
    Addr addr = add ? (base + imm) : (base - imm);

    DPRINTF(MProfileFP, "MFpStrS::initiateAcc: s%d [r%d%s%d] "
            "base=%#x imm=%d add=%d addr=%#x\n",
            sd, rn, add ? "+" : "-", imm, base, imm, add, addr);

    uint32_t data = (uint32_t)xc->getRegOperand(this, 1);
    return gem5::writeMemTimingLE(xc, traceData, data, addr,
                                  Request::Flags(0), nullptr);
}

Fault
MFpStrS::completeAcc(PacketPtr pkt, ExecContext *xc,
                      trace::InstRecord *traceData) const
{
    return NoFault;
}

std::string
MFpStrS::generateDisassembly(Addr pc,
    const loader::SymbolTable *symtab) const
{
    std::ostringstream ss;
    ss << "vstr s" << sd << ", [r" << rn
       << ", #" << (add ? "+" : "-") << imm << "]";
    return ss.str();
}

// =====================================================================
// MFpLdrD — VLDR.64 Dd, [Rn, #imm]
// Loads 8 bytes as a single 64-bit read.
// D<n> aliases {S<2n+1>, S<2n>}: low word → S<2n>, high → S<2n+1>.
// =====================================================================

Fault
MFpLdrD::execute(ExecContext *xc, trace::InstRecord *traceData) const
{
    ThreadContext *tc = xc->tcBase();
    Fault fault = checkMProfileFPEnabled(tc);
    if (fault != NoFault) return fault;
    setFPCA(tc);

    // PC-relative base for VLDR (literal). Thumb semantics (ARM ARM
    // A6.1.2): the architected PC inside any instruction is
    // `instruction_address + 4`, and VLDR (literal) then Align()s it
    // to 4. `instAddr()` returns the raw instruction address without
    // the +4 pipeline offset, so we add 4 explicitly before aligning.
    // Skipping the +4 pulled the wrong word from the literal pool
    // (e.g. Eigen's gebp zero-accumulator load was reading garbage,
    // poisoning every VFMA — see tinympc iter1 blowup trace).
    Addr base = (rn == int_reg::Pc)
        ? ((tc->pcState().as<ArmISA::PCState>().instAddr() + 4) & ~0x3)
        : tc->getReg(RegId(intRegClass, rn));
    Addr addr = add ? (base + imm) : (base - imm);

    uint64_t data = 0;
    fault = gem5::readMemAtomicLE(xc, traceData, addr, data,
                                  Request::Flags(0));
    if (fault != NoFault) return fault;

    xc->setRegOperand(this, 0, (RegVal)(uint32_t)(data & 0xFFFFFFFF));
    xc->setRegOperand(this, 1, (RegVal)(uint32_t)(data >> 32));
    return NoFault;
}

Fault
MFpLdrD::initiateAcc(ExecContext *xc, trace::InstRecord *traceData) const
{
    ThreadContext *tc = xc->tcBase();
    Fault fault = checkMProfileFPEnabled(tc);
    if (fault != NoFault) return fault;
    setFPCA(tc);

    // PC-relative base for VLDR (literal). Thumb semantics (ARM ARM
    // A6.1.2): the architected PC inside any instruction is
    // `instruction_address + 4`, and VLDR (literal) then Align()s it
    // to 4. `instAddr()` returns the raw instruction address without
    // the +4 pipeline offset, so we add 4 explicitly before aligning.
    // Skipping the +4 pulled the wrong word from the literal pool
    // (e.g. Eigen's gebp zero-accumulator load was reading garbage,
    // poisoning every VFMA — see tinympc iter1 blowup trace).
    Addr base = (rn == int_reg::Pc)
        ? ((tc->pcState().as<ArmISA::PCState>().instAddr() + 4) & ~0x3)
        : tc->getReg(RegId(intRegClass, rn));
    Addr addr = add ? (base + imm) : (base - imm);

    DPRINTF(MProfileFP, "MFpLdrD::initiateAcc: d%d [r%d%s%d] "
            "rn_is_pc=%d base=%#x imm=%d add=%d addr=%#x\n",
            dd, rn, add ? "+" : "-", imm,
            (rn == int_reg::Pc), base, imm, add, addr);

    uint64_t dummy = 0;
    return gem5::initiateMemRead(xc, traceData, addr, dummy,
                                 Request::Flags(0));
}

Fault
MFpLdrD::completeAcc(PacketPtr pkt, ExecContext *xc,
                      trace::InstRecord *traceData) const
{
    uint64_t data = 0;
    gem5::getMemLE(pkt, data, traceData);
    xc->setRegOperand(this, 0, (RegVal)(uint32_t)(data & 0xFFFFFFFF));
    xc->setRegOperand(this, 1, (RegVal)(uint32_t)(data >> 32));
    return NoFault;
}

std::string
MFpLdrD::generateDisassembly(Addr pc,
    const loader::SymbolTable *symtab) const
{
    std::ostringstream ss;
    ss << "vldr d" << dd << ", [r" << rn
       << ", #" << (add ? "+" : "-") << imm << "]";
    return ss.str();
}

// =====================================================================
// MFpStrD — VSTR.64 Dd, [Rn, #imm]
// Stores 8 bytes as a single 64-bit write.
// =====================================================================

Fault
MFpStrD::execute(ExecContext *xc, trace::InstRecord *traceData) const
{
    ThreadContext *tc = xc->tcBase();
    Fault fault = checkMProfileFPEnabled(tc);
    if (fault != NoFault) return fault;
    setFPCA(tc);

    Addr base = tc->getReg(RegId(intRegClass, rn));
    Addr addr = add ? (base + imm) : (base - imm);

    uint32_t word1 = (uint32_t)xc->getRegOperand(this, 1);
    uint32_t word2 = (uint32_t)xc->getRegOperand(this, 2);
    uint64_t data = ((uint64_t)word2 << 32) | word1;

    return gem5::writeMemAtomicLE(xc, traceData, data, addr,
                                  Request::Flags(0), nullptr);
}

Fault
MFpStrD::initiateAcc(ExecContext *xc, trace::InstRecord *traceData) const
{
    ThreadContext *tc = xc->tcBase();
    Fault fault = checkMProfileFPEnabled(tc);
    if (fault != NoFault) return fault;
    setFPCA(tc);

    Addr base = tc->getReg(RegId(intRegClass, rn));
    Addr addr = add ? (base + imm) : (base - imm);

    uint32_t word1 = (uint32_t)xc->getRegOperand(this, 1);
    uint32_t word2 = (uint32_t)xc->getRegOperand(this, 2);
    uint64_t data = ((uint64_t)word2 << 32) | word1;

    DPRINTF(MProfileFP, "MFpStrD::initiateAcc: d%d [r%d%s%d] "
            "base=%#x imm=%d add=%d addr=%#x data=%#x\n",
            dd, rn, add ? "+" : "-", imm,
            base, imm, add, addr, data);

    return gem5::writeMemTimingLE(xc, traceData, data, addr,
                                  Request::Flags(0), nullptr);
}

Fault
MFpStrD::completeAcc(PacketPtr pkt, ExecContext *xc,
                      trace::InstRecord *traceData) const
{
    return NoFault;
}

std::string
MFpStrD::generateDisassembly(Addr pc,
    const loader::SymbolTable *symtab) const
{
    std::ostringstream ss;
    ss << "vstr d" << dd << ", [r" << rn
       << ", #" << (add ? "+" : "-") << imm << "]";
    return ss.str();
}

// =====================================================================
// MMacroVFPMemOp — M-profile VLDM/VSTM/VPUSH/VPOP
//
// Same expansion logic as MacroVFPMemOp (macromem.cc) but uses
// MFpLdrS/MFpStrS/MFpLdrD/MFpStrD micro-ops with M-profile FP
// checking instead of A-profile MicroLdr/StrFpUop which call
// checkAdvSIMDOrFPEnabled32.
// =====================================================================

MMacroVFPMemOp::MMacroVFPMemOp(const char *mnem, ExtMachInst machInst,
                               OpClass __opClass, RegIndex rn,
                               RegIndex vd, bool single, bool up,
                               bool writeback, bool load,
                               uint32_t offset)
    : PredMacroOp(mnem, machInst, __opClass)
{
    // -----------------------------------------------------------------
    // Unified SP/DP micro-op expansion (single-word, 4 bytes per µop).
    //
    // History: the original expansion emitted one `MFpLdrD`/`MFpStrD`
    // per D-register. Each of those micro-ops wrote TWO `vecElemClass`
    // destinations (low S(2n) + high S(2n+1)). For a range like
    // {d0-d3}, pairs of D-registers land in the same underlying NEON
    // vector register (D0+D1 both in V0, D2+D3 both in V1), and the
    // partial-V-register writes from consecutive multi-dest µops were
    // ordering-sensitive — their final state leaked bits from the host
    // (observed as `s4=garbage`, bit-identical within one host but
    // different across hosts).
    //
    // Fix: always emit one `MFpLdrS`/`MFpStrS` per 4-byte block. Each
    // µop has a single `vecElemClass` dest/src, so there is no partial
    // V-register write. This matches the A-profile A7.7.234 expansion
    // pattern (MicroLdrDBFpUop + MicroLdrDTFpUop) which is known-good.
    //
    // Register numbering:
    //  - SP:  decoder passes `vd = S_base`, so `vd + j` walks S-regs.
    //  - DP:  decoder passes `vd = D_base * 2` (the s-index of the
    //         low half of the base D-reg). `vd + j` therefore walks
    //         S(2*D_base), S(2*D_base+1), S(2*(D_base+1)), … which is
    //         D_base low, D_base high, D_base+1 low, … as required.
    //
    // imm8 accounting:
    //  - SP: imm8 = number of S-regs = number of 4-byte blocks.
    //  - DP: imm8 = 2 * number_of_D_regs = number of 4-byte blocks.
    //  - Deprecated FLDMIAX/FSTMIAX (DP, imm8 odd): we emit the
    //    even portion (`offset & ~1`) of 4-byte blocks; writeback
    //    still uses `4 * offset` so Rn lands on the ARM-specified
    //    post-access address (including the 4 extra untouched bytes).
    // -----------------------------------------------------------------

    // Number of 4-byte blocks to actually load/store.
    int count_s = single ? offset : (offset & ~1);
    numMicroops = count_s + (writeback ? 1 : 0);
    microOps = new StaticInstPtr[numMicroops];

    // Address sequencing:
    //  - up (IA, increment after): blocks at base+0, +4, +8, …
    //  - !up (DB, decrement before, e.g. VPUSH): blocks at
    //    base-totalBytes, base-totalBytes+4, … (lowest address first,
    //    ascending) — Rn is decremented by the writeback µop.
    // `MFpLdrS`/`MFpStrS` take an unsigned imm + `add` flag, so we
    // split the signed per-µop offset into (absOff, thisAdd) here.
    int32_t totalBytes = 4 * offset;
    int32_t startOffset = up ? 0 : -totalBytes;

    int i = 0;
    for (int j = 0; j < count_s; j++) {
        int32_t thisOffset = startOffset + j * 4;
        RegIndex sd = vd + j;
        bool thisAdd = (thisOffset >= 0);
        int32_t absOff = thisAdd ? thisOffset : -thisOffset;
        if (load) {
            microOps[i] = new MFpLdrS(machInst, sd, rn,
                                       absOff, thisAdd);
        } else {
            microOps[i] = new MFpStrS(machInst, sd, rn,
                                       absOff, thisAdd);
        }

        microOps[i]->setFlag(StaticInst::IsMicroop);
        i++;
    }

    if (writeback) {
        int32_t wb_imm = up ? (4 * offset) : -(int32_t)(4 * offset);
        microOps[i] = new MFpWritebackUop(machInst, rn, wb_imm);
        microOps[i]->setFlag(StaticInst::IsMicroop);
        i++;
    }

    assert(numMicroops == i);
    microOps[0]->setFirstMicroop();
    microOps[numMicroops - 1]->setLastMicroop();

    for (int k = 0; k < numMicroops - 1; k++) {
        microOps[k]->setDelayedCommit();
    }
}

std::string
MMacroVFPMemOp::generateDisassembly(
    Addr pc, const loader::SymbolTable *symtab) const
{
    return csprintf("%-10s (M-profile VFP multi-reg mem op)", mnemonic);
}

} // namespace ArmISA
} // namespace gem5
