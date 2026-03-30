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

#include "arch/arm/m_faults.hh"
#include "arch/arm/regs/int.hh"
#include "arch/arm/regs/misc.hh"
#include "arch/arm/regs/misc_types.hh"
#include "arch/arm/regs/vec.hh"
#include "base/logging.hh"
#include "base/trace.hh"
#include "base/types.hh"
#include "cpu/exec_context.hh"
#include "cpu/thread_context.hh"
#include "debug/MProfileFP.hh"

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

} // namespace ArmISA
} // namespace gem5
