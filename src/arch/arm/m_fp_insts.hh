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
// MFpCmpS — VCMP.F32, VCMPE.F32
// src: S[op1], S[op2] (or just op1 if withZero), FPSCR
// dest: FPSCR
// =====================================================================

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

} // namespace ArmISA
} // namespace gem5

#endif // __ARCH_ARM_M_FP_INSTS_HH__
