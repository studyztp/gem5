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

#ifndef __ARCH_ARM_M_INSTS_HH__
#define __ARCH_ARM_M_INSTS_HH__

/** @file
 * M-profile instruction classes.
 *
 * These classes are returned by MDecoder::tryMProfileDecode() for
 * instructions that have different behavior on M-profile versus
 * A-profile.  They access only M-profile registers (MISCREG_M_*)
 * and never touch CPSR/SPSR.
 *
 * Each class documents the ARMv7-M reference for that instruction.
 */

#include "arch/arm/insts/pred_inst.hh"
#include "arch/arm/regs/misc.hh"

namespace gem5
{

namespace ArmISA
{

/**
 * M-profile MRS: Move from Special Register.
 *
 * MRS Rd, <spec_reg>
 * Thumb-32 encoding T1: 11110011111 (0)SYSm[7:0] Rd[11:8] 10(0)00000
 *
 * The SYSm field (bits[7:0] of the instruction, mapped to
 * bits[11:8] and [4] by the encoding) selects which M-profile
 * special register to read:
 *
 *   SYSm      Register       Reference
 *   ────      ────────       ─────────
 *   0b00000   APSR           DDI0403E B5.2.1
 *   0b00001   IAPSR          DDI0403E B5.2.1
 *   0b00010   EAPSR          DDI0403E B5.2.1
 *   0b00011   XPSR           DDI0403E B5.2.1
 *   0b00101   IPSR           DDI0403E B5.2.1
 *   0b00110   EPSR           DDI0403E B5.2.1
 *   0b00111   IEPSR          DDI0403E B5.2.1
 *   0b01000   MSP            DDI0403E B5.2.2
 *   0b01001   PSP            DDI0403E B5.2.2
 *   0b10000   PRIMASK        DDI0403E B5.2.3
 *   0b10001   BASEPRI        DDI0403E B5.2.4 (ARMv7-M only)
 *   0b10010   BASEPRI_MAX    DDI0403E B5.2.5 (ARMv7-M only)
 *   0b10011   FAULTMASK      DDI0403E B5.2.6 (ARMv7-M only)
 *   0b10100   CONTROL        DDI0403E B5.2.7
 *
 * Reference: DDI0403E A7.7.41 (MRS)
 */
class MrsMProfile : public PredOp
{
  protected:
    RegIndex dest;
    uint8_t sysM;

  public:
    MrsMProfile(ExtMachInst mach_inst, RegIndex _dest, uint8_t _sysM)
        : PredOp("mrs", mach_inst, IntAluOp),
          dest(_dest), sysM(_sysM)
    {}

    Fault execute(ExecContext *xc,
                  trace::InstRecord *traceData) const override;

    std::string generateDisassembly(
            Addr pc,
            const loader::SymbolTable *symtab) const override;
};

/**
 * M-profile MSR: Move to Special Register.
 *
 * MSR <spec_reg>, Rn
 * Thumb-32 encoding T1: 111100111000 Rn[19:16] 10(0)0 SYSm[7:0]
 *
 * Same SYSm encoding as MRS (see above).
 *
 * Reference: DDI0403E A7.7.42 (MSR)
 */
class MsrMProfile : public PredOp
{
  protected:
    RegIndex op1;
    uint8_t sysM;

  public:
    MsrMProfile(ExtMachInst mach_inst, RegIndex _op1, uint8_t _sysM)
        : PredOp("msr", mach_inst, IntAluOp),
          op1(_op1), sysM(_sysM)
    {}

    Fault execute(ExecContext *xc,
                  trace::InstRecord *traceData) const override;

    std::string generateDisassembly(
            Addr pc,
            const loader::SymbolTable *symtab) const override;
};

/**
 * M-profile CPS: Change Processor State.
 *
 * CPSID i / CPSIE i — set/clear PRIMASK
 * CPSID f / CPSIE f — set/clear FAULTMASK (ARMv7-M only)
 *
 * Thumb-16 encoding T1: 10110110011 im[4] (0)(0) F[1] I[0]
 *   im=1: disable (set mask)
 *   im=0: enable (clear mask)
 *   I: affects PRIMASK
 *   F: affects FAULTMASK
 *
 * M-profile CPS does NOT change mode bits (no CPSR.mode on M-profile).
 *
 * Reference: DDI0403E A7.7.17 (CPS), B5.2.3 (PRIMASK), B5.2.6 (FAULTMASK)
 */
class CpsMProfile : public PredOp
{
  protected:
    bool disable;  // true = CPSID (set mask), false = CPSIE (clear mask)
    bool affectI;  // true = modify PRIMASK
    bool affectF;  // true = modify FAULTMASK

  public:
    CpsMProfile(ExtMachInst mach_inst,
                bool _disable, bool _affectI, bool _affectF)
        : PredOp("cps", mach_inst, IntAluOp),
          disable(_disable), affectI(_affectI), affectF(_affectF)
    {}

    Fault execute(ExecContext *xc,
                  trace::InstRecord *traceData) const override;

    std::string generateDisassembly(
            Addr pc,
            const loader::SymbolTable *symtab) const override;
};

/**
 * M-profile BX: Branch and Exchange with EXC_RETURN detection.
 *
 * BX Rm
 * Thumb-16 encoding T1: 01000111 0 Rm[6:3] (000)
 *
 * If the target address matches the EXC_RETURN pattern
 * (addr & 0xFFFFFF00) == 0xFFFFFF00, this is NOT a branch — it
 * triggers M-profile exception return (unstacking).
 *
 * EXC_RETURN values (DDI0403E B1.5.8):
 *   0xFFFFFFF1 — Return to Handler mode, use MSP
 *   0xFFFFFFF9 — Return to Thread mode, use MSP
 *   0xFFFFFFFD — Return to Thread mode, use PSP
 *
 * If not EXC_RETURN: normal branch to Rm.
 * Bit[0] of target sets Thumb state (always 1 on M-profile).
 *
 * Reference: DDI0403E A7.7.20 (BX), B1.5.8 (exception return)
 */
class BxMProfile : public PredOp
{
  protected:
    RegIndex op1;

  public:
    BxMProfile(ExtMachInst mach_inst, RegIndex _op1)
        : PredOp("bx", mach_inst, IntAluOp),
          op1(_op1)
    {
        flags[IsIndirectControl] = true;
        flags[IsUncondControl] = true;
    }

    Fault execute(ExecContext *xc,
                  trace::InstRecord *traceData) const override;

    std::string generateDisassembly(
            Addr pc,
            const loader::SymbolTable *symtab) const override;
};

/**
 * M-profile BLX reg: Branch with Link and Exchange + EXC_RETURN.
 *
 * BLX Rm
 * Thumb-16 encoding T1: 01000111 1 Rm[6:3] (000)
 *
 * Same EXC_RETURN detection as BX.  Additionally sets LR to the
 * return address (next instruction after BLX).
 *
 * Reference: DDI0403E A7.7.19 (BLX register)
 */
class BlxRegMProfile : public PredOp
{
  protected:
    RegIndex op1;

  public:
    BlxRegMProfile(ExtMachInst mach_inst, RegIndex _op1)
        : PredOp("blx", mach_inst, IntAluOp),
          op1(_op1)
    {
        flags[IsIndirectControl] = true;
        flags[IsUncondControl] = true;
        flags[IsCall] = true;
    }

    Fault execute(ExecContext *xc,
                  trace::InstRecord *traceData) const override;

    std::string generateDisassembly(
            Addr pc,
            const loader::SymbolTable *symtab) const override;
};

/**
 * M-profile SVC: Supervisor Call.
 *
 * SVC #imm8
 * Thumb-16 encoding T1: 11011111 imm[7:0]
 *
 * Same encoding as A-profile SVC, but generates ArmMFault(MPEXC_SVCALL)
 * instead of A-profile SupervisorCall fault.
 *
 * Reference: DDI0403E A7.7.175 (SVC), B1.5.2 (exception entry)
 */
class SvcMProfile : public PredOp
{
  protected:
    uint8_t svcImm;

  public:
    SvcMProfile(ExtMachInst mach_inst, uint8_t _imm)
        : PredOp("svc", mach_inst, IntAluOp),
          svcImm(_imm)
    {
        flags[IsSyscall] = true;
    }

    Fault execute(ExecContext *xc,
                  trace::InstRecord *traceData) const override;

    std::string generateDisassembly(
            Addr pc,
            const loader::SymbolTable *symtab) const override;
};

/**
 * M-profile WFI: Wait for Interrupt.
 *
 * M-profile WFI checks for pending unmasked interrupts via the SCS
 * (not via A-profile HCR/CPSR/SCR trap checks).  If no interrupt is
 * pending, the CPU sleeps until one arrives.
 *
 * Reference: DDI0403E A7.7.184 (WFI)
 */
class WfiMProfile : public PredOp
{
  public:
    WfiMProfile(ExtMachInst mach_inst)
        : PredOp("wfi", mach_inst, IntAluOp)
    {
        flags[IsQuiesce] = true;
    }

    Fault execute(ExecContext *xc,
                  trace::InstRecord *traceData) const override;

    std::string generateDisassembly(
            Addr pc,
            const loader::SymbolTable *symtab) const override;
};

/**
 * M-profile WFE: Wait for Event.
 *
 * M-profile WFE checks the event register.  If set, clears it and
 * returns.  If not set, sleeps until an event (SEV from another
 * core, interrupt, or debug event).
 *
 * Reference: DDI0403E A7.7.183 (WFE)
 */
class WfeMProfile : public PredOp
{
  public:
    WfeMProfile(ExtMachInst mach_inst)
        : PredOp("wfe", mach_inst, IntAluOp)
    {
        flags[IsQuiesce] = true;
    }

    Fault execute(ExecContext *xc,
                  trace::InstRecord *traceData) const override;

    std::string generateDisassembly(
            Addr pc,
            const loader::SymbolTable *symtab) const override;
};

/**
 * M-profile undefined instruction.
 *
 * Used for A-profile-only instructions that are undefined on M-profile:
 * SMC, HVC, ERET, BXJ, SRS, RFE, SUBS PC LR, MRS/MSR SPSR,
 * MRS/MSR Banked, SETEND, MRC/MCR CP14/CP15.
 *
 * Returns ArmMFault(MPEXC_USAGEFAULT) — the M-profile equivalent of
 * an undefined instruction exception.
 *
 * Reference: DDI0403E B1.5.3 (UsageFault), B3.2.15 (CFSR.UNDEFINSTR)
 */
class MProfileUndefined : public PredOp
{
  protected:
    const char *reason;

  public:
    MProfileUndefined(ExtMachInst mach_inst, const char *_reason)
        : PredOp("undefined", mach_inst, IntAluOp),
          reason(_reason)
    {
        flags[IsInvalid] = true;
    }

    Fault execute(ExecContext *xc,
                  trace::InstRecord *traceData) const override;

    std::string generateDisassembly(
            Addr pc,
            const loader::SymbolTable *symtab) const override;
};

/**
 * M-profile exception return triggered by POP {PC} / LDM {PC}.
 *
 * When a load instruction writes an EXC_RETURN value to PC, the
 * CPU tries to fetch from that address (0xFFFFFFF_).  MDecoder
 * detects this in moreBytes() and returns this synthetic instruction
 * instead.  Its execute() calls mProfileExcReturn() to perform the
 * actual exception return (unstacking).
 *
 * This handles the common pattern in Cortex-M handlers:
 *   PUSH {r4-r7, lr}    ; save callee-saved + EXC_RETURN
 *   ...handler code...
 *   POP  {r4-r7, pc}    ; restore + return via EXC_RETURN
 *
 * Reference: DDI0403E B1.5.8 (exception return)
 */
class ExcReturnFromPC : public PredOp
{
  protected:
    uint32_t excReturnVal;

  public:
    ExcReturnFromPC(ExtMachInst mach_inst, uint32_t _excReturn)
        : PredOp("exc_return", mach_inst, IntAluOp),
          excReturnVal(_excReturn)
    {}

    Fault execute(ExecContext *xc,
                  trace::InstRecord *traceData) const override;

    std::string generateDisassembly(
            Addr pc,
            const loader::SymbolTable *symtab) const override;
};

} // namespace ArmISA
} // namespace gem5

#endif // __ARCH_ARM_M_INSTS_HH__
