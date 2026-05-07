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

#ifndef __ARCH_ARM_M_FP_INSTS_HH__
#define __ARCH_ARM_M_FP_INSTS_HH__

/**
 * @file
 * M-profile floating-point instruction classes.
 *
 * These replace the A-profile VFP instruction implementations (from
 * vfp.isa) for M-profile cores.  The A-profile versions call
 * checkAdvSIMDOrFPEnabled32() which crashes on M-profile because
 * ArmMSystem does not inherit from ArmSystem.
 *
 * M-profile FP instructions differ from A-profile in three ways:
 *   1. Access control: CPACR.CP10/CP11 only (no EL2/EL3 traps)
 *   2. Every FP instruction sets CONTROL.FPCA = 1 [DDI0403E B1.4.2]
 *   3. Lazy stacking: check FPCCR.LSPEN for deferred FP save
 *
 * Design: MFpOp base class handles the common wrapper (enable check,
 * FPCA, lazy stacking).  Children override doFpOp() for the actual
 * computation.  Each child declares srcRegIdxArr/destRegIdxArr so the
 * MinorCPU scoreboard can track VFP register dependencies for correct
 * pipeline timing.
 */

#include <cmath>
#include <type_traits>

#include "arch/arm/insts/pred_inst.hh"
#include "arch/arm/m_insts.hh"     // mProfilePredicateHolds
#include "arch/arm/pcstate.hh"
#include "arch/arm/regs/cc.hh"
#include "arch/arm/regs/int.hh"
#include "arch/arm/regs/misc.hh"
#include "arch/arm/regs/misc_info.hh"
#include "arch/arm/regs/vec.hh"
#include "cpu/exec_context.hh"
#include "cpu/op_class.hh"

namespace gem5
{

namespace ArmISA
{

// =====================================================================
// Helper: map VFP single-precision register index (0-31) to a RegId
// in gem5's vec elem register class.
//
// S0-S3 = elements 0-3 of V0, S4-S7 = elements 0-3 of V1, etc.
// =====================================================================

inline RegId
vfpSRegId(RegIndex idx)
{
    return vecElemClass[(idx / 4) * NumVecElemPerNeonVecReg + idx % 4];
}

// =====================================================================
// Host FP environment verification.
// =====================================================================

void verifyHostFpEnvironment();

// =====================================================================
// Trivial FP operation wrappers.
// =====================================================================

inline float mFpAdd(float a, float b) { return a + b; }
inline float mFpSub(float a, float b) { return a - b; }
inline float mFpMul(float a, float b) { return a * b; }
inline float mFpDiv(float a, float b) { return a / b; }
inline float mFpNeg(float a) { return -a; }
inline float mFpAbs(float a) { return std::fabs(a); }
inline float mFpSqrt(float a) { return std::sqrt(a); }

// =====================================================================
// M-profile FP computation helpers.
// =====================================================================

float mFpBinaryOp(FPSCR &fpscr, float op1, float op2,
                  float (*func)(float, float));

float mFpUnaryOp(FPSCR &fpscr, float op1,
                 float (*func)(float));

// =====================================================================
// MFpOp — Base class for all M-profile FP instructions.
// =====================================================================

class MFpOp : public PredOp
{
  protected:
    Fault checkMProfileFPEnabled(ThreadContext *tc) const;
    void setFPCA(ThreadContext *tc) const;
    Fault completeLazyStacking(ThreadContext *tc) const;

    virtual Fault doFpOp(ExecContext *xc,
                         trace::InstRecord *traceData) const = 0;

  public:
    MFpOp(const char *mnem, ExtMachInst mach_inst, OpClass op_class)
        : PredOp(mnem, mach_inst, op_class)
    {
        flags[IsVectorElem] = true;
    }

    Fault execute(ExecContext *xc,
                  trace::InstRecord *traceData) const override;

    // Micro-op aware PC advancement.  When MFpOp subclasses are used
    // as micro-ops inside MMacroVFPMemOp, they must step through the
    // macroop's micro-op sequence rather than advancing the main PC.
    void
    advancePC(PCStateBase &pcState) const override
    {
        auto &apc = pcState.as<PCState>();
        if (flags[IsLastMicroop]) {
            apc.uEnd();
        } else if (flags[IsMicroop]) {
            apc.uAdvance();
        } else {
            apc.advance();
        }
    }

    void
    advancePC(ThreadContext *tc) const override
    {
        PCState pc = tc->pcState().as<PCState>();
        advancePC(pc);
        tc->pcState(pc);
    }
};

// =====================================================================
// MFpBinS — Binary single-precision (VADD, VSUB, VMUL, VDIV, etc.)
// src: S[op1], S[op2], FPSCR   dest: S[dest], FPSCR
// =====================================================================

using FpBinFunc = float (*)(float, float);

class MFpBinS : public MFpOp
{
  private:
    RegId srcRegIdxArr[3];
    RegId destRegIdxArr[2];

  protected:
    RegIndex dest, op1, op2;
    FpBinFunc func;
    const char *opName;

    Fault doFpOp(ExecContext *xc,
                 trace::InstRecord *traceData) const override;

  public:
    MFpBinS(const char *mnem, ExtMachInst mach_inst, OpClass op_class,
            RegIndex _dest, RegIndex _op1, RegIndex _op2,
            FpBinFunc _func, const char *_opName = "")
        : MFpOp(mnem, mach_inst, op_class),
          dest(_dest), op1(_op1), op2(_op2),
          func(_func), opName(_opName)
    {
        setRegIdxArrays(
            reinterpret_cast<RegIdArrayPtr>(
                &std::remove_pointer_t<decltype(this)>::srcRegIdxArr),
            reinterpret_cast<RegIdArrayPtr>(
                &std::remove_pointer_t<decltype(this)>::destRegIdxArr));

        setSrcRegIdx(_numSrcRegs++, vfpSRegId(op1));
        setSrcRegIdx(_numSrcRegs++, vfpSRegId(op2));
        setSrcRegIdx(_numSrcRegs++, miscRegClass[MISCREG_FPSCR]);
        setDestRegIdx(_numDestRegs++, vfpSRegId(dest));
        _numTypedDestRegs[vecElemClass.type()]++;
        setDestRegIdx(_numDestRegs++, miscRegClass[MISCREG_FPSCR]);
        _numTypedDestRegs[miscRegClass.type()]++;
    }

    std::string generateDisassembly(
        Addr pc, const loader::SymbolTable *symtab) const override;
};

// =====================================================================
// MFpTernaryS — Ternary single-precision (VFMA, VFMS, VFNMA, VFNMS)
// Computes dest = func(dest, op1, op2), e.g. VFMA: Sd += Sn * Sm
// src: S[dest], S[op1], S[op2], FPSCR   dest: S[dest], FPSCR
// =====================================================================

using FpTernaryFunc = float (*)(float, float, float);

class MFpTernaryS : public MFpOp
{
  private:
    RegId srcRegIdxArr[4];
    RegId destRegIdxArr[2];

  protected:
    RegIndex dest, op1, op2;
    FpTernaryFunc func;

    Fault doFpOp(ExecContext *xc,
                 trace::InstRecord *traceData) const override;

  public:
    MFpTernaryS(const char *mnem, ExtMachInst mach_inst,
                OpClass op_class, RegIndex _dest, RegIndex _op1,
                RegIndex _op2, FpTernaryFunc _func)
        : MFpOp(mnem, mach_inst, op_class),
          dest(_dest), op1(_op1), op2(_op2), func(_func)
    {
        setRegIdxArrays(
            reinterpret_cast<RegIdArrayPtr>(
                &std::remove_pointer_t<decltype(this)>::srcRegIdxArr),
            reinterpret_cast<RegIdArrayPtr>(
                &std::remove_pointer_t<decltype(this)>::destRegIdxArr));

        setSrcRegIdx(_numSrcRegs++, vfpSRegId(dest));  // accumulator
        setSrcRegIdx(_numSrcRegs++, vfpSRegId(op1));
        setSrcRegIdx(_numSrcRegs++, vfpSRegId(op2));
        setSrcRegIdx(_numSrcRegs++, miscRegClass[MISCREG_FPSCR]);
        setDestRegIdx(_numDestRegs++, vfpSRegId(dest));
        _numTypedDestRegs[vecElemClass.type()]++;
        setDestRegIdx(_numDestRegs++, miscRegClass[MISCREG_FPSCR]);
        _numTypedDestRegs[miscRegClass.type()]++;
    }

    std::string generateDisassembly(
        Addr pc, const loader::SymbolTable *symtab) const override;
};

// =====================================================================
// MFpFusedMulAddS — Ternary single-precision fused multiply-add
// (VFMA, VFMS, VFNMA, VFNMS)
//
// Unlike MFpTernaryS (which uses host `*`/`+` via a C++ lambda and
// therefore rounds TWICE), this class invokes gem5's pure-software
// `fplibMulAdd<uint32_t>` (see src/arch/arm/insts/fplib.cc:2211) so
// the fused multiply-add is computed with a SINGLE rounding, matching
// ARM VFMA.F32 / VFMS.F32 / VFNMA.F32 / VFNMS.F32 semantics.
//
// fplibMulAdd operates on IEEE 754 bit patterns and handles FPSCR.FZ/
// DN/RMode, NaN propagation, INF*0 invalid, and denormal flush
// internally — no host FP state is read. This makes results
// bit-identical across hosts, which VMLA/VMLS (still on MFpTernaryS)
// do not guarantee.
//
// Variant is encoded as two booleans that XOR the sign bit of the
// addend (Sd) and/or first multiplicand (Sn), matching ARM's FPNeg
// (which only flips bit 31, preserving NaN payload):
//
//   Mnem    ARM pseudocode                      negateAddend  negateProduct
//   VFMA    FPMulAdd(Sd, Sn, Sm)                false         false
//   VFMS    FPMulAdd(Sd, FPNeg(Sn), Sm)         false         true
//   VFNMS   FPMulAdd(FPNeg(Sd), Sn, Sm)         true          false
//   VFNMA   FPMulAdd(FPNeg(Sd), FPNeg(Sn), Sm)  true          true
//
// src: S[dest], S[op1], S[op2], FPSCR   dest: S[dest], FPSCR
// =====================================================================

class MFpFusedMulAddS : public MFpOp
{
  private:
    RegId srcRegIdxArr[4];
    RegId destRegIdxArr[2];

  protected:
    RegIndex dest, op1, op2;
    // XOR mask applied to addend (Sd) sign bit: true for VFNMS, VFNMA.
    bool negateAddend;
    // XOR mask applied to first multiplicand (Sn) sign bit so the
    // product is negated inside the fused mul-add: true for VFMS, VFNMA.
    bool negateProduct;

    Fault doFpOp(ExecContext *xc,
                 trace::InstRecord *traceData) const override;

  public:
    MFpFusedMulAddS(const char *mnem, ExtMachInst mach_inst,
                    OpClass op_class, RegIndex _dest, RegIndex _op1,
                    RegIndex _op2, bool _negateAddend,
                    bool _negateProduct)
        : MFpOp(mnem, mach_inst, op_class),
          dest(_dest), op1(_op1), op2(_op2),
          negateAddend(_negateAddend),
          negateProduct(_negateProduct)
    {
        setRegIdxArrays(
            reinterpret_cast<RegIdArrayPtr>(
                &std::remove_pointer_t<decltype(this)>::srcRegIdxArr),
            reinterpret_cast<RegIdArrayPtr>(
                &std::remove_pointer_t<decltype(this)>::destRegIdxArr));

        // Sd is both source (accumulator) and destination — same shape
        // as MFpTernaryS, so dispatch / dependency tracking in the
        // Minor CPU sees an identical ternary-µop profile.
        setSrcRegIdx(_numSrcRegs++, vfpSRegId(dest));  // accumulator
        setSrcRegIdx(_numSrcRegs++, vfpSRegId(op1));
        setSrcRegIdx(_numSrcRegs++, vfpSRegId(op2));
        setSrcRegIdx(_numSrcRegs++, miscRegClass[MISCREG_FPSCR]);
        setDestRegIdx(_numDestRegs++, vfpSRegId(dest));
        _numTypedDestRegs[vecElemClass.type()]++;
        setDestRegIdx(_numDestRegs++, miscRegClass[MISCREG_FPSCR]);
        _numTypedDestRegs[miscRegClass.type()]++;
    }

    std::string generateDisassembly(
        Addr pc, const loader::SymbolTable *symtab) const override;
};

// =====================================================================
// MFpUnaryS — Unary single-precision (VNEG, VABS, VSQRT)
// src: S[op1], FPSCR   dest: S[dest], FPSCR
// =====================================================================

using FpUnaryFunc = float (*)(float);

class MFpUnaryS : public MFpOp
{
  private:
    RegId srcRegIdxArr[2];
    RegId destRegIdxArr[2];

  protected:
    RegIndex dest, op1;
    FpUnaryFunc func;

    Fault doFpOp(ExecContext *xc,
                 trace::InstRecord *traceData) const override;

  public:
    MFpUnaryS(const char *mnem, ExtMachInst mach_inst, OpClass op_class,
              RegIndex _dest, RegIndex _op1, FpUnaryFunc _func)
        : MFpOp(mnem, mach_inst, op_class),
          dest(_dest), op1(_op1), func(_func)
    {
        setRegIdxArrays(
            reinterpret_cast<RegIdArrayPtr>(
                &std::remove_pointer_t<decltype(this)>::srcRegIdxArr),
            reinterpret_cast<RegIdArrayPtr>(
                &std::remove_pointer_t<decltype(this)>::destRegIdxArr));

        setSrcRegIdx(_numSrcRegs++, vfpSRegId(op1));
        setSrcRegIdx(_numSrcRegs++, miscRegClass[MISCREG_FPSCR]);
        setDestRegIdx(_numDestRegs++, vfpSRegId(dest));
        _numTypedDestRegs[vecElemClass.type()]++;
        setDestRegIdx(_numDestRegs++, miscRegClass[MISCREG_FPSCR]);
        _numTypedDestRegs[miscRegClass.type()]++;
    }

    std::string generateDisassembly(
        Addr pc, const loader::SymbolTable *symtab) const override;
};

// =====================================================================
// MFpMovImmS — VMOV.F32 Sd, #imm
// src: (none)   dest: S[dest]
// =====================================================================

class MFpMovImmS : public MFpOp
{
  private:
    RegId srcRegIdxArr[1];  // unused but needed for setRegIdxArrays
    RegId destRegIdxArr[1];

  protected:
    RegIndex dest;
    uint32_t imm;

    Fault doFpOp(ExecContext *xc,
                 trace::InstRecord *traceData) const override;

  public:
    MFpMovImmS(ExtMachInst mach_inst, RegIndex _dest, uint32_t _imm)
        : MFpOp("vmov.f32", mach_inst, SimdFloatMiscOp),
          dest(_dest), imm(_imm)
    {
        setRegIdxArrays(
            reinterpret_cast<RegIdArrayPtr>(
                &std::remove_pointer_t<decltype(this)>::srcRegIdxArr),
            reinterpret_cast<RegIdArrayPtr>(
                &std::remove_pointer_t<decltype(this)>::destRegIdxArr));

        setDestRegIdx(_numDestRegs++, vfpSRegId(dest));
        _numTypedDestRegs[vecElemClass.type()]++;
    }

    std::string generateDisassembly(
        Addr pc, const loader::SymbolTable *symtab) const override;
};

// =====================================================================
// MFpMovRegS — VMOV.F32 Sd, Sm
// src: S[op1]   dest: S[dest]
// =====================================================================

class MFpMovRegS : public MFpOp
{
  private:
    RegId srcRegIdxArr[1];
    RegId destRegIdxArr[1];

  protected:
    RegIndex dest, op1;

    Fault doFpOp(ExecContext *xc,
                 trace::InstRecord *traceData) const override;

  public:
    MFpMovRegS(ExtMachInst mach_inst, RegIndex _dest, RegIndex _op1)
        : MFpOp("vmov.f32", mach_inst, SimdFloatMiscOp),
          dest(_dest), op1(_op1)
    {
        setRegIdxArrays(
            reinterpret_cast<RegIdArrayPtr>(
                &std::remove_pointer_t<decltype(this)>::srcRegIdxArr),
            reinterpret_cast<RegIdArrayPtr>(
                &std::remove_pointer_t<decltype(this)>::destRegIdxArr));

        setSrcRegIdx(_numSrcRegs++, vfpSRegId(op1));
        setDestRegIdx(_numDestRegs++, vfpSRegId(dest));
        _numTypedDestRegs[vecElemClass.type()]++;
    }

    std::string generateDisassembly(
        Addr pc, const loader::SymbolTable *symtab) const override;
};

// =====================================================================
// MFpMovCoreToS — VMOV Sn, Rd (core → VFP)
// src: R[rt]   dest: S[sd]
// =====================================================================

class MFpMovCoreToS : public MFpOp
{
  private:
    RegId srcRegIdxArr[1];
    RegId destRegIdxArr[1];

  protected:
    RegIndex sd, rt;

    Fault doFpOp(ExecContext *xc,
                 trace::InstRecord *traceData) const override;

  public:
    MFpMovCoreToS(ExtMachInst mach_inst, RegIndex _sd, RegIndex _rt)
        : MFpOp("vmov", mach_inst, SimdFloatMiscOp),
          sd(_sd), rt(_rt)
    {
        setRegIdxArrays(
            reinterpret_cast<RegIdArrayPtr>(
                &std::remove_pointer_t<decltype(this)>::srcRegIdxArr),
            reinterpret_cast<RegIdArrayPtr>(
                &std::remove_pointer_t<decltype(this)>::destRegIdxArr));

        setSrcRegIdx(_numSrcRegs++, intRegClass[rt]);
        setDestRegIdx(_numDestRegs++, vfpSRegId(sd));
        _numTypedDestRegs[vecElemClass.type()]++;
    }

    std::string generateDisassembly(
        Addr pc, const loader::SymbolTable *symtab) const override;
};

// =====================================================================
// MFpMovSToCore — VMOV Rd, Sn (VFP → core)
// src: S[sn]   dest: R[rt]
// =====================================================================

class MFpMovSToCore : public MFpOp
{
  private:
    RegId srcRegIdxArr[1];
    RegId destRegIdxArr[1];

  protected:
    RegIndex rt, sn;

    Fault doFpOp(ExecContext *xc,
                 trace::InstRecord *traceData) const override;

  public:
    MFpMovSToCore(ExtMachInst mach_inst, RegIndex _rt, RegIndex _sn)
        : MFpOp("vmov", mach_inst, SimdFloatMiscOp),
          rt(_rt), sn(_sn)
    {
        setRegIdxArrays(
            reinterpret_cast<RegIdArrayPtr>(
                &std::remove_pointer_t<decltype(this)>::srcRegIdxArr),
            reinterpret_cast<RegIdArrayPtr>(
                &std::remove_pointer_t<decltype(this)>::destRegIdxArr));

        setSrcRegIdx(_numSrcRegs++, vfpSRegId(sn));
        setDestRegIdx(_numDestRegs++, intRegClass[rt]);
        _numTypedDestRegs[intRegClass.type()]++;
    }

    std::string generateDisassembly(
        Addr pc, const loader::SymbolTable *symtab) const override;
};

// =====================================================================
// MFpMovCorePairToD — VMOV Dm, Rt, Rt2
// Two core registers → one D-register (doubleword data transfer).
// Valid on FPv4-SP [DDI0403 A6.3].
// =====================================================================

class MFpMovCorePairToD : public MFpOp
{
  private:
    RegId srcRegIdxArr[2];
    RegId destRegIdxArr[2];

  protected:
    RegIndex dd, rt, rt2;

    Fault doFpOp(ExecContext *xc,
                 trace::InstRecord *traceData) const override;

  public:
    MFpMovCorePairToD(ExtMachInst mach_inst, RegIndex _dd,
                      RegIndex _rt, RegIndex _rt2)
        : MFpOp("vmov", mach_inst, SimdFloatMiscOp),
          dd(_dd), rt(_rt), rt2(_rt2)
    {
        setRegIdxArrays(
            reinterpret_cast<RegIdArrayPtr>(
                &std::remove_pointer_t<decltype(this)>::srcRegIdxArr),
            reinterpret_cast<RegIdArrayPtr>(
                &std::remove_pointer_t<decltype(this)>::destRegIdxArr));

        setSrcRegIdx(_numSrcRegs++, intRegClass[rt]);
        setSrcRegIdx(_numSrcRegs++, intRegClass[rt2]);
        setDestRegIdx(_numDestRegs++, vfpSRegId(dd * 2));
        _numTypedDestRegs[vecElemClass.type()]++;
        setDestRegIdx(_numDestRegs++, vfpSRegId(dd * 2 + 1));
        _numTypedDestRegs[vecElemClass.type()]++;
    }

    std::string generateDisassembly(
        Addr pc, const loader::SymbolTable *symtab) const override;
};

// =====================================================================
// MFpMovDToCorePair — VMOV Rt, Rt2, Dm
// One D-register → two core registers.
// =====================================================================

class MFpMovDToCorePair : public MFpOp
{
  private:
    RegId srcRegIdxArr[2];
    RegId destRegIdxArr[2];

  protected:
    RegIndex dd, rt, rt2;

    Fault doFpOp(ExecContext *xc,
                 trace::InstRecord *traceData) const override;

  public:
    MFpMovDToCorePair(ExtMachInst mach_inst, RegIndex _dd,
                      RegIndex _rt, RegIndex _rt2)
        : MFpOp("vmov", mach_inst, SimdFloatMiscOp),
          dd(_dd), rt(_rt), rt2(_rt2)
    {
        setRegIdxArrays(
            reinterpret_cast<RegIdArrayPtr>(
                &std::remove_pointer_t<decltype(this)>::srcRegIdxArr),
            reinterpret_cast<RegIdArrayPtr>(
                &std::remove_pointer_t<decltype(this)>::destRegIdxArr));

        setSrcRegIdx(_numSrcRegs++, vfpSRegId(dd * 2));
        setSrcRegIdx(_numSrcRegs++, vfpSRegId(dd * 2 + 1));
        setDestRegIdx(_numDestRegs++, intRegClass[rt]);
        setDestRegIdx(_numDestRegs++, intRegClass[rt2]);
    }

    std::string generateDisassembly(
        Addr pc, const loader::SymbolTable *symtab) const override;
};

// =====================================================================
// MFpCmpS — VCMP.F32, VCMPE.F32
// src: S[op1], S[op2] (or just op1 if withZero), FPSCR
// dest: FPSCR
// =====================================================================

// =====================================================================
// MFpCvtS — VCVT integer ↔ float single-precision
// Handles VCVT.F32.U32, VCVT.F32.S32, VCVT.U32.F32, VCVT.S32.F32.
// Does NOT use mFpUnaryOp because:
//   - int→float: input is integer bits, NOT a float (denormal flush
//     would corrupt it); uses round-to-nearest, not FPSCR.RMode
//   - float→int: uses round-towards-zero, not FPSCR.RMode
// =====================================================================

class MFpCvtS : public MFpOp
{
  private:
    RegId srcRegIdxArr[2];
    RegId destRegIdxArr[2];

  protected:
    RegIndex dest, op1;
    bool toFloat;   // true: int→float, false: float→int
    bool isSigned;  // true: S32, false: U32

    Fault doFpOp(ExecContext *xc,
                 trace::InstRecord *traceData) const override;

  public:
    MFpCvtS(ExtMachInst mach_inst, RegIndex _dest, RegIndex _op1,
            bool _toFloat, bool _isSigned)
        : MFpOp("vcvt", mach_inst, SimdFloatCvtOp),
          dest(_dest), op1(_op1),
          toFloat(_toFloat), isSigned(_isSigned)
    {
        setRegIdxArrays(
            reinterpret_cast<RegIdArrayPtr>(
                &std::remove_pointer_t<decltype(this)>::srcRegIdxArr),
            reinterpret_cast<RegIdArrayPtr>(
                &std::remove_pointer_t<decltype(this)>::destRegIdxArr));

        setSrcRegIdx(_numSrcRegs++, vfpSRegId(op1));
        setSrcRegIdx(_numSrcRegs++, miscRegClass[MISCREG_FPSCR]);
        setDestRegIdx(_numDestRegs++, vfpSRegId(dest));
        _numTypedDestRegs[vecElemClass.type()]++;
        setDestRegIdx(_numDestRegs++, miscRegClass[MISCREG_FPSCR]);
        _numTypedDestRegs[miscRegClass.type()]++;
    }

    std::string generateDisassembly(
        Addr pc, const loader::SymbolTable *symtab) const override;
};

// =====================================================================
// MFpCvtFixedS — VCVT between single-precision float and fixed-point.
// Handles the four #imm-fbits encodings (Armv7-M ARM A7.7.219):
//   VCVT.F32.S<w>   Sd, Sd, #fbits  (signed fixed → float)    opc2=0xa
//   VCVT.F32.U<w>   Sd, Sd, #fbits  (unsigned fixed → float)  opc2=0xb
//   VCVT.S<w>.F32   Sd, Sd, #fbits  (float → signed fixed)    opc2=0xf
//   VCVT.U<w>.F32   Sd, Sd, #fbits  (float → unsigned fixed)  opc2=0xe
// where w = intWidth ∈ {16, 32} (sx flag = bit 7).
//
// Replaces the A-profile VcvtSFixedFpS / VcvtUFixedFpS / VcvtFpSFixedS /
// VcvtFpUFixedS path, which calls checkAdvSIMDOrFPEnabled32() and
// segfaults on M-profile (ArmMSystem doesn't inherit from ArmSystem,
// so getArmSystem() does a UB static_cast).  The MFpOp base wraps
// doFpOp() with the M-profile FP-enable check (CPACR.cp10/cp11),
// CONTROL.FPCA, and lazy stacking — none of which need ArmSystem.
//
// On the encoding, source and destination registers are the same Vd,
// so we keep both fields for symmetry with MFpCvtS (and so the
// dependency tracker sees a read-then-write on Sd).
// =====================================================================

class MFpCvtFixedS : public MFpOp
{
  private:
    RegId srcRegIdxArr[2];
    RegId destRegIdxArr[2];

  protected:
    RegIndex dest, op1;
    bool toFloat;       // true: fixed → float, false: float → fixed
    bool isSigned;      // true: S<w>, false: U<w>
    uint8_t intWidth;   // 16 (sx=0) or 32 (sx=1)
    uint8_t fbits;      // fractional bits, derived from sx and i:imm4

    Fault doFpOp(ExecContext *xc,
                 trace::InstRecord *traceData) const override;

  public:
    MFpCvtFixedS(ExtMachInst mach_inst, RegIndex _dest, RegIndex _op1,
                 bool _toFloat, bool _isSigned,
                 uint8_t _intWidth, uint8_t _fbits)
        : MFpOp("vcvt", mach_inst, SimdFloatCvtOp),
          dest(_dest), op1(_op1),
          toFloat(_toFloat), isSigned(_isSigned),
          intWidth(_intWidth), fbits(_fbits)
    {
        setRegIdxArrays(
            reinterpret_cast<RegIdArrayPtr>(
                &std::remove_pointer_t<decltype(this)>::srcRegIdxArr),
            reinterpret_cast<RegIdArrayPtr>(
                &std::remove_pointer_t<decltype(this)>::destRegIdxArr));

        setSrcRegIdx(_numSrcRegs++, vfpSRegId(op1));
        setSrcRegIdx(_numSrcRegs++, miscRegClass[MISCREG_FPSCR]);
        setDestRegIdx(_numDestRegs++, vfpSRegId(dest));
        _numTypedDestRegs[vecElemClass.type()]++;
        setDestRegIdx(_numDestRegs++, miscRegClass[MISCREG_FPSCR]);
        _numTypedDestRegs[miscRegClass.type()]++;
    }

    std::string generateDisassembly(
        Addr pc, const loader::SymbolTable *symtab) const override;
};

// =====================================================================
// MFpCmpS — VCMP.F32, VCMPE.F32
class MFpCmpS : public MFpOp
{
  private:
    RegId srcRegIdxArr[3];
    RegId destRegIdxArr[1];

  protected:
    RegIndex op1, op2;
    bool withExc;
    bool withZero;

    Fault doFpOp(ExecContext *xc,
                 trace::InstRecord *traceData) const override;

  public:
    MFpCmpS(ExtMachInst mach_inst, RegIndex _op1, RegIndex _op2,
            bool _withExc, bool _withZero)
        : MFpOp("vcmp.f32", mach_inst, SimdFloatCmpOp),
          op1(_op1), op2(_op2), withExc(_withExc), withZero(_withZero)
    {
        setRegIdxArrays(
            reinterpret_cast<RegIdArrayPtr>(
                &std::remove_pointer_t<decltype(this)>::srcRegIdxArr),
            reinterpret_cast<RegIdArrayPtr>(
                &std::remove_pointer_t<decltype(this)>::destRegIdxArr));

        setSrcRegIdx(_numSrcRegs++, vfpSRegId(op1));
        if (!withZero)
            setSrcRegIdx(_numSrcRegs++, vfpSRegId(op2));
        setSrcRegIdx(_numSrcRegs++, miscRegClass[MISCREG_FPSCR]);
        setDestRegIdx(_numDestRegs++, miscRegClass[MISCREG_FPSCR]);
        _numTypedDestRegs[miscRegClass.type()]++;
    }

    std::string generateDisassembly(
        Addr pc, const loader::SymbolTable *symtab) const override;
};

// =====================================================================
// MFpMrs — VMRS Rd, FPSCR (or APSR_nzcv)
// src: FPSCR   dest: R[rt] (or CC regs if rt==0xF)
// =====================================================================

class MFpMrs : public MFpOp
{
  private:
    RegId srcRegIdxArr[1];
    RegId destRegIdxArr[4];  // up to 3 CC regs + 1 int reg

  protected:
    RegIndex rt;

    Fault doFpOp(ExecContext *xc,
                 trace::InstRecord *traceData) const override;

  public:
    MFpMrs(ExtMachInst mach_inst, RegIndex _rt)
        : MFpOp("vmrs", mach_inst, SimdFloatMiscOp),
          rt(_rt)
    {
        setRegIdxArrays(
            reinterpret_cast<RegIdArrayPtr>(
                &std::remove_pointer_t<decltype(this)>::srcRegIdxArr),
            reinterpret_cast<RegIdArrayPtr>(
                &std::remove_pointer_t<decltype(this)>::destRegIdxArr));

        setSrcRegIdx(_numSrcRegs++, miscRegClass[MISCREG_FPSCR]);
        if (rt == 0xF) {
            // VMRS APSR_nzcv, FPSCR — writes CC regs
            setDestRegIdx(_numDestRegs++, ccRegClass[cc_reg::Nz]);
            _numTypedDestRegs[ccRegClass.type()]++;
            setDestRegIdx(_numDestRegs++, ccRegClass[cc_reg::C]);
            _numTypedDestRegs[ccRegClass.type()]++;
            setDestRegIdx(_numDestRegs++, ccRegClass[cc_reg::V]);
            _numTypedDestRegs[ccRegClass.type()]++;
        } else {
            setDestRegIdx(_numDestRegs++, intRegClass[rt]);
            _numTypedDestRegs[intRegClass.type()]++;
        }
    }

    std::string generateDisassembly(
        Addr pc, const loader::SymbolTable *symtab) const override;
};

// =====================================================================
// MFpMsr — VMSR FPSCR, Rd
// src: R[rt]   dest: FPSCR
// =====================================================================

class MFpMsr : public MFpOp
{
  private:
    RegId srcRegIdxArr[1];
    RegId destRegIdxArr[1];

  protected:
    RegIndex rt;

    Fault doFpOp(ExecContext *xc,
                 trace::InstRecord *traceData) const override;

  public:
    MFpMsr(ExtMachInst mach_inst, RegIndex _rt)
        : MFpOp("vmsr", mach_inst, SimdFloatMiscOp),
          rt(_rt)
    {
        setRegIdxArrays(
            reinterpret_cast<RegIdArrayPtr>(
                &std::remove_pointer_t<decltype(this)>::srcRegIdxArr),
            reinterpret_cast<RegIdArrayPtr>(
                &std::remove_pointer_t<decltype(this)>::destRegIdxArr));

        setSrcRegIdx(_numSrcRegs++, intRegClass[rt]);
        setDestRegIdx(_numDestRegs++, miscRegClass[MISCREG_FPSCR]);
        _numTypedDestRegs[miscRegClass.type()]++;
    }

    std::string generateDisassembly(
        Addr pc, const loader::SymbolTable *symtab) const override;
};

// =====================================================================
// MFpLdrS — VLDR.32 Sd, [Rn, #imm]
// Single-precision FP load (4 bytes).  2 cycles [DDI0439D Table 7-1].
// =====================================================================

class MFpLdrS : public MFpOp
{
  private:
    RegId srcRegIdxArr[1];
    RegId destRegIdxArr[1];

  protected:
    RegIndex sd;
    RegIndex rn;
    int32_t imm;
    bool add;

    // Not used — execute/initiateAcc/completeAcc override MFpOp flow.
    Fault doFpOp(ExecContext *, trace::InstRecord *) const override
    { return NoFault; }

  public:
    MFpLdrS(ExtMachInst mach_inst, RegIndex _sd, RegIndex _rn,
            int32_t _imm, bool _add)
        : MFpOp("vldr.32", mach_inst, FloatMemReadOp),
          sd(_sd), rn(_rn), imm(_imm), add(_add)
    {
        setRegIdxArrays(
            reinterpret_cast<RegIdArrayPtr>(
                &std::remove_pointer_t<decltype(this)>::srcRegIdxArr),
            reinterpret_cast<RegIdArrayPtr>(
                &std::remove_pointer_t<decltype(this)>::destRegIdxArr));

        setSrcRegIdx(_numSrcRegs++, intRegClass[rn]);
        setDestRegIdx(_numDestRegs++, vfpSRegId(sd));
        _numTypedDestRegs[vecElemClass.type()]++;

        flags[IsLoad] = true;
    }

    Fault execute(ExecContext *xc,
                  trace::InstRecord *traceData) const override;
    Fault initiateAcc(ExecContext *xc,
                      trace::InstRecord *traceData) const override;
    Fault completeAcc(PacketPtr pkt, ExecContext *xc,
                      trace::InstRecord *traceData) const override;

    std::string generateDisassembly(
        Addr pc, const loader::SymbolTable *symtab) const override;
};

// =====================================================================
// MFpStrS — VSTR.32 Sd, [Rn, #imm]
// Single-precision FP store (4 bytes).  2 cycles [DDI0439D Table 7-1].
// =====================================================================

class MFpStrS : public MFpOp
{
  private:
    RegId srcRegIdxArr[2];
    RegId destRegIdxArr[1];

  protected:
    RegIndex sd;
    RegIndex rn;
    int32_t imm;
    bool add;

    Fault doFpOp(ExecContext *, trace::InstRecord *) const override
    { return NoFault; }

  public:
    MFpStrS(ExtMachInst mach_inst, RegIndex _sd, RegIndex _rn,
            int32_t _imm, bool _add)
        : MFpOp("vstr.32", mach_inst, FloatMemWriteOp),
          sd(_sd), rn(_rn), imm(_imm), add(_add)
    {
        setRegIdxArrays(
            reinterpret_cast<RegIdArrayPtr>(
                &std::remove_pointer_t<decltype(this)>::srcRegIdxArr),
            reinterpret_cast<RegIdArrayPtr>(
                &std::remove_pointer_t<decltype(this)>::destRegIdxArr));

        setSrcRegIdx(_numSrcRegs++, intRegClass[rn]);
        setSrcRegIdx(_numSrcRegs++, vfpSRegId(sd));

        flags[IsStore] = true;
    }

    Fault execute(ExecContext *xc,
                  trace::InstRecord *traceData) const override;
    Fault initiateAcc(ExecContext *xc,
                      trace::InstRecord *traceData) const override;
    Fault completeAcc(PacketPtr pkt, ExecContext *xc,
                      trace::InstRecord *traceData) const override;

    std::string generateDisassembly(
        Addr pc, const loader::SymbolTable *symtab) const override;
};

// =====================================================================
// MFpLdrD — VLDR.64 Dd, [Rn, #imm]
// Double-word FP load (8 bytes = 2 x 4B reads).  3 cycles.
// Valid on FPv4-SP: "supports doubleword data transfer instructions"
// [DDI0403 A6.3].  D<n> aliases {S<2n+1>, S<2n>}.
// =====================================================================

class MFpLdrD : public MFpOp
{
  private:
    RegId srcRegIdxArr[1];
    RegId destRegIdxArr[2];

  protected:
    RegIndex dd;
    RegIndex rn;
    int32_t imm;
    bool add;

    Fault doFpOp(ExecContext *, trace::InstRecord *) const override
    { return NoFault; }

  public:
    MFpLdrD(ExtMachInst mach_inst, RegIndex _dd, RegIndex _rn,
            int32_t _imm, bool _add)
        : MFpOp("vldr.64", mach_inst, FloatMemReadOp),
          dd(_dd), rn(_rn), imm(_imm), add(_add)
    {
        setRegIdxArrays(
            reinterpret_cast<RegIdArrayPtr>(
                &std::remove_pointer_t<decltype(this)>::srcRegIdxArr),
            reinterpret_cast<RegIdArrayPtr>(
                &std::remove_pointer_t<decltype(this)>::destRegIdxArr));

        setSrcRegIdx(_numSrcRegs++, intRegClass[rn]);
        setDestRegIdx(_numDestRegs++, vfpSRegId(dd * 2));
        _numTypedDestRegs[vecElemClass.type()]++;
        setDestRegIdx(_numDestRegs++, vfpSRegId(dd * 2 + 1));
        _numTypedDestRegs[vecElemClass.type()]++;

        flags[IsLoad] = true;
    }

    Fault execute(ExecContext *xc,
                  trace::InstRecord *traceData) const override;
    Fault initiateAcc(ExecContext *xc,
                      trace::InstRecord *traceData) const override;
    Fault completeAcc(PacketPtr pkt, ExecContext *xc,
                      trace::InstRecord *traceData) const override;

    std::string generateDisassembly(
        Addr pc, const loader::SymbolTable *symtab) const override;
};

// =====================================================================
// MFpStrD — VSTR.64 Dd, [Rn, #imm]
// Double-word FP store (8 bytes = 2 x 4B writes).  3 cycles.
// =====================================================================

class MFpStrD : public MFpOp
{
  private:
    RegId srcRegIdxArr[3];
    RegId destRegIdxArr[1];

  protected:
    RegIndex dd;
    RegIndex rn;
    int32_t imm;
    bool add;

    Fault doFpOp(ExecContext *, trace::InstRecord *) const override
    { return NoFault; }

  public:
    MFpStrD(ExtMachInst mach_inst, RegIndex _dd, RegIndex _rn,
            int32_t _imm, bool _add)
        : MFpOp("vstr.64", mach_inst, FloatMemWriteOp),
          dd(_dd), rn(_rn), imm(_imm), add(_add)
    {
        setRegIdxArrays(
            reinterpret_cast<RegIdArrayPtr>(
                &std::remove_pointer_t<decltype(this)>::srcRegIdxArr),
            reinterpret_cast<RegIdArrayPtr>(
                &std::remove_pointer_t<decltype(this)>::destRegIdxArr));

        setSrcRegIdx(_numSrcRegs++, intRegClass[rn]);
        setSrcRegIdx(_numSrcRegs++, vfpSRegId(dd * 2));
        setSrcRegIdx(_numSrcRegs++, vfpSRegId(dd * 2 + 1));

        flags[IsStore] = true;
    }

    Fault execute(ExecContext *xc,
                  trace::InstRecord *traceData) const override;
    Fault initiateAcc(ExecContext *xc,
                      trace::InstRecord *traceData) const override;
    Fault completeAcc(PacketPtr pkt, ExecContext *xc,
                      trace::InstRecord *traceData) const override;

    std::string generateDisassembly(
        Addr pc, const loader::SymbolTable *symtab) const override;
};

// =====================================================================
// MFpWritebackUop — simple Rn += imm writeback micro-op
// Used by MMacroVFPMemOp for VLDM/VSTM writeback.
// =====================================================================

class MFpWritebackUop : public PredOp
{
  private:
    RegId srcRegIdxArr[1];
    RegId destRegIdxArr[1];

  protected:
    RegIndex rn;
    int32_t imm;

  public:
    MFpWritebackUop(ExtMachInst mach_inst, RegIndex _rn, int32_t _imm)
        : PredOp("vfp_wb", mach_inst, IntAluOp),
          rn(_rn), imm(_imm)
    {
        setRegIdxArrays(
            reinterpret_cast<RegIdArrayPtr>(
                &std::remove_pointer_t<decltype(this)>::srcRegIdxArr),
            reinterpret_cast<RegIdArrayPtr>(
                &std::remove_pointer_t<decltype(this)>::destRegIdxArr));

        setSrcRegIdx(_numSrcRegs++, intRegClass[rn]);
        setDestRegIdx(_numDestRegs++, intRegClass[rn]);
    }

    Fault execute(ExecContext *xc,
                  trace::InstRecord *traceData) const override
    {
        ThreadContext *tc = xc->tcBase();
        // ITSTATE predication: if the parent VLDM/VSTM was predicated
        // and the condition is false, the writeback must NOT happen.
        // ARM ARM: predicated VLDM/VSTM has no effect on Rn either.
        if (!mProfilePredicateHolds(tc, condCode)) return NoFault;
        Addr base = tc->getReg(RegId(intRegClass, rn));
        xc->setRegOperand(this, 0, (RegVal)(base + imm));
        return NoFault;
    }

    std::string generateDisassembly(
        Addr pc, const loader::SymbolTable *symtab) const override
    {
        std::ostringstream ss;
        ss << "vfp_wb r" << rn << ", #" << imm;
        return ss.str();
    }

    void
    advancePC(PCStateBase &pcState) const override
    {
        auto &apc = pcState.as<PCState>();
        if (flags[IsLastMicroop]) {
            apc.uEnd();
        } else if (flags[IsMicroop]) {
            apc.uAdvance();
        } else {
            apc.advance();
        }
    }

    void
    advancePC(ThreadContext *tc) const override
    {
        PCState pc = tc->pcState().as<PCState>();
        advancePC(pc);
        tc->pcState(pc);
    }
};

// =====================================================================
// MMacroVFPMemOp — M-profile VLDM/VSTM/VPUSH/VPOP
//
// Macroop that expands into MFpLdrS/MFpStrS (single) or
// MFpLdrD/MFpStrD (double) micro-ops with M-profile FP checks.
// Replaces the A-profile MacroVFPMemOp which uses micro-ops
// that call checkAdvSIMDOrFPEnabled32 (crashes on M-profile).
// =====================================================================

class MMacroVFPMemOp : public PredMacroOp
{
  public:
    MMacroVFPMemOp(const char *mnem, ExtMachInst machInst,
                   OpClass __opClass, RegIndex rn, RegIndex vd,
                   bool single, bool up, bool writeback,
                   bool load, uint32_t offset);

    std::string generateDisassembly(
        Addr pc, const loader::SymbolTable *symtab) const override;
};

} // namespace ArmISA
} // namespace gem5

#endif // __ARCH_ARM_M_FP_INSTS_HH__
