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

#include "arch/arm/m_decoder.hh"

#include "arch/arm/m_fp_insts.hh"
#include "arch/arm/m_insts.hh"
#include "base/bitfield.hh"
#include "debug/Decode.hh"

namespace gem5
{

namespace ArmISA
{

MDecoder::MDecoder(const Params &params)
    : InstDecoder(params, &data),
      decoderFlavor(enums::Generic),  // M-profile uses Generic flavor
      dvmEnabled(false),              // M-profile has no DVM
      mSystem(dynamic_cast<ArmMSystem *>(params.system)),
      data(0), bigThumb(false), offset(0), foundIt(false)
{
    // If the release includes FPU support, verify the host FP environment
    // can correctly simulate ARM FPv4-SP single-precision operations.
    if (has(ArmExtension::M_PROFILE_FPU_SP)) {
        verifyHostFpEnvironment();
    }

    reset();
}

// =========================================================================
// Thumb instruction assembly — replicated from Decoder (decoder.cc:81-185)
// No A-profile ISA dependencies.  M-profile is always Thumb.
// =========================================================================

void
MDecoder::reset()
{
    InstDecoder::reset();
    bigThumb = false;
    offset = 0;
    emi = 0;
    foundIt = false;
}

void
MDecoder::process()
{
    // Instruction ready unless we're mid-way through a 32-bit Thumb.
    instDone = true;

    // M-profile is always Thumb — no ARM mode path needed.
    uint16_t word = (data >> (offset * 8));
    if (bigThumb) {
        // Second half of a 32-bit Thumb instruction.
        emi.instBits = emi.instBits | word;
        bigThumb = false;
        consumeBytes(2);
        DPRINTF(Decode, "MDecoder: second half 32-bit Thumb: %#x\n",
                emi.instBits);
    } else {
        uint16_t highBits = word & 0xF800;
        if (highBits == 0xE800 || highBits == 0xF000 ||
                highBits == 0xF800) {
            // Start of a 32-bit Thumb instruction.
            emi.bigThumb = 1;
            if (offset == 0) {
                // Both halfwords available in the fetch data.
                emi.instBits = (data >> 16) | (data << 16);
                DPRINTF(Decode, "MDecoder: full 32-bit Thumb: %#x\n",
                        emi.instBits);
                consumeBytes(4);
            } else {
                // Only have the first halfword.
                emi.instBits = (uint32_t)word << 16;
                bigThumb = true;
                consumeBytes(2);
                instDone = false;  // Need second halfword.
            }
        } else {
            // 16-bit Thumb instruction.
            consumeBytes(2);
            emi.instBits = word;
            emi.condCode = COND_UC;
            DPRINTF(Decode, "MDecoder: 16-bit Thumb: %#x\n",
                    emi.instBits);
            // IT instruction detection
            if (bits(word, 15, 8) == 0xbf &&
                    bits(word, 3, 0) != 0x0) {
                foundIt = true;
                itBits = bits(word, 7, 0);
                DPRINTF(Decode, "MDecoder: IT detected, "
                        "cond=%#x, mask=%#x\n",
                        itBits.cond, itBits.mask);
            }
        }
    }
}

void
MDecoder::consumeBytes(int numBytes)
{
    offset += numBytes;
    assert(offset <= sizeof(data) || emi.decoderFault);
    if (offset == sizeof(data))
        outOfBytes = true;
}

void
MDecoder::moreBytes(const PCStateBase &_pc, Addr fetchPC)
{
    auto &pc = _pc.as<PCState>();

    // EXC_RETURN detection for POP {PC} / LDM {PC}:
    // When an LDM/POP micro-op loads an EXC_RETURN value into PC,
    // the CPU tries to fetch the next instruction from that address
    // (e.g., 0xFFFFFFF8).  We detect this here and set a flag
    // so that decode() returns an exception-return instruction
    // instead of trying to decode from that address.
    //
    // EXC_RETURN values have bits[31:4] all set (DDI0403E B1.5.8).
    // Valid range: 0xFFFFFFF0–0xFFFFFFFF.
    if ((pc.instAddr() & 0xFFFFFFF0) == 0xFFFFFFF0) {
        _pendingExcReturn = true;
        _excReturnVal = (uint32_t)pc.instAddr();
        // Don't try to fetch — the data at 0xFFFFFFF_ is unmapped.
        // Set instDone so decode() can handle it.
        instDone = true;
        return;
    }
    _pendingExcReturn = false;

    data = letoh(data);
    offset = (fetchPC >= pc.instAddr()) ? 0 : pc.instAddr() - fetchPC;

    // M-profile is always Thumb. Set these fields for the decode tree.
    emi.thumb = 1;       // Always Thumb on M-profile
    emi.aarch64 = 0;     // Never AArch64 on M-profile
    emi.fpscrLen = 0;    // No FPSCR vector length on M-profile
    emi.fpscrStride = 0;
    emi.sveLen = 0;      // No SVE on M-profile

    const Addr alignment = 0x1;  // Thumb: 2-byte aligned
    emi.decoderFault = static_cast<uint8_t>(
        pc.instAddr() & alignment ?
            DecoderFault::UNALIGNED : DecoderFault::OK);

    outOfBytes = false;
    process();
}

// =========================================================================
// decode() — M-profile override of InstDecoder::decode
// =========================================================================
//
// Called by the CPU pipeline at every instruction boundary.
//
// Flow:
//   1. Try M-profile decode: match encoding against instructions
//      that differ on M-profile (see tryMProfileDecode)
//   2. If matched: return M-profile instruction object
//   3. If not matched: fall through to Decoder::decode(pc) which
//      uses the standard ISA-generated A-profile Thumb decoder
//
// The fall-through is safe because MISA aliases CPSR → xPSR:
//   - Load/store instructions read CPSR.e (bit[9]) for endianness
//     → alias returns xPSR bit[9] = 0 → correct (always LE)
//   - DSP saturation instructions write CPSR.Q (bit[27])
//     → MISA redirects to xPSR bit[27] → correct
//   - No other CPSR field is accessed by fall-through instructions
//   See step8_cpsr_alias_proof.md for the complete proof.

StaticInstPtr
MDecoder::decode(PCStateBase &_pc)
{
    // Must replicate the housekeeping that Decoder::decode does
    // (isa.cc line 188-211): instDone check, PC advancement,
    // IT state handling, decoder state reset.  If we just called
    // Decoder::decode for the fall-through case, the M-profile
    // intercept path would skip all of this.

    if (!instDone)
        return NULL;

    auto &pc = _pc.as<PCState>();

    // Handle POP {PC} / LDM {PC} EXC_RETURN.
    // moreBytes() detected that the fetch address is an EXC_RETURN
    // value (0xFFFFFFF_) and set _pendingExcReturn.  Return a
    // BxMProfile instruction that will call mProfileExcReturn().
    if (_pendingExcReturn) {
        _pendingExcReturn = false;
        instDone = false;
        // Create a BxMProfile that will trigger exception return.
        // We pass the EXC_RETURN value via a synthetic instruction
        // that reads LR (which still holds the EXC_RETURN value
        // on M-profile when returning via POP {PC}).
        // Actually, the EXC_RETURN value is the PC itself.
        // Create an instruction that calls mProfileExcReturn directly.
        StaticInstPtr inst = new ExcReturnFromPC(
            ExtMachInst(0), _excReturnVal);
        inst->size(0);  // No actual instruction bytes consumed
        return inst;
    }

    // Advance PC and handle IT state (same as Decoder::decode)
    const int inst_size((!emi.thumb || emi.bigThumb) ? 4 : 2);
    ExtMachInst this_emi(emi);

    pc.npc(pc.pc() + inst_size);
    if (foundIt)
        pc.nextItstate(itBits);
    this_emi.itstate = pc.itstate();
    this_emi.illegalExecution = pc.illegalExec() ? 1 : 0;
    this_emi.debugStep = pc.debugStep() ? 1 : 0;
    pc.size(inst_size);

    // Reset decoder state (consumed this instruction)
    emi = 0;
    instDone = false;
    foundIt = false;

    // Try M-profile decode first
    StaticInstPtr inst = tryMProfileDecode(this_emi);

    if (inst) {
        inst->size(inst_size);
        DPRINTF(Decode, "MDecoder: M-profile decoded %s: %#x\n",
                inst->getName(), (uint64_t)this_emi);
        return inst;
    }

    // Fall through to standard Thumb decoder (ISA-generated decode tree).
    // Safe because MISA aliases CPSR → xPSR (step8_cpsr_alias_proof.md).
    StaticInstPtr si = decodeThumbFallthrough(this_emi, pc.instAddr());
    si->size(inst_size);
    return si;
}

// =========================================================================
// tryMProfileDecode — main dispatch
// =========================================================================

StaticInstPtr
MDecoder::tryMProfileDecode(ExtMachInst mach_inst)
{
    if (!mach_inst.thumb) {
        // M-profile is always Thumb.  If somehow we're not in Thumb
        // mode, something is very wrong.  Let the standard decoder
        // handle it (it will likely fault on ARM-mode decode).
        return nullptr;
    }

    if (mach_inst.bigThumb) {
        // 32-bit Thumb instruction
        return tryMProfileDecode32(mach_inst);
    } else {
        // 16-bit Thumb instruction
        return tryMProfileDecode16(mach_inst);
    }
}

// =========================================================================
// tryMProfileDecode16 — 16-bit Thumb intercepts
// =========================================================================
//
// Encoding reference: DDI0403E A5.2 (16-bit Thumb instruction encoding)
//
// Instructions intercepted:
//   BX Rm      — [15:7] = 010001110  → EXC_RETURN detection
//   BLX Rm     — [15:7] = 010001111  → EXC_RETURN detection
//   CPS        — [15:5] = 10110110011 → PRIMASK/FAULTMASK
//   SVC imm8   — [15:8] = 11011111   → M-profile SVCALL exception
//   BKPT #0xAB — inst = 0xBEAB       → M-profile semihosting
//   WFE        — hint instruction     → M-profile sleep semantics
//   WFI        — hint instruction     → M-profile sleep semantics
//   POP {PC}   — [15:9] = 1011110, bit[8]=1 → EXC_RETURN check

StaticInstPtr
MDecoder::tryMProfileDecode16(ExtMachInst mach_inst)
{
    const uint32_t inst = (uint32_t)mach_inst;

    // ---- BX Rm / BLX Rm ----
    // Encoding: 010001 11 L Rm[6:3] 000
    //   bits[15:8] = 01000111
    //   bit[7] = L (0=BX, 1=BLX)
    //   bits[6:3] = Rm
    // gem5 A-profile: formats/data.isa:1076-1084
    // DDI0403E A7.7.20 (BX), A7.7.19 (BLX register)
    if (bits(inst, 15, 8) == 0x47) {
        const RegIndex rm = (RegIndex)bits(inst, 6, 3);
        if (bits(inst, 7) == 0) {
            return new BxMProfile(mach_inst, rm);
        } else {
            return new BlxRegMProfile(mach_inst, rm);
        }
    }

    // ---- SVC #imm8 ----
    // Encoding: 1101 1111 imm[7:0]
    //   bits[15:8] = 0xDF
    // gem5 A-profile: formats/branch.isa:101-102
    // DDI0403E A7.7.175 (SVC)
    if (bits(inst, 15, 8) == 0xDF) {
        const uint8_t imm8 = bits(inst, 7, 0);
        return new SvcMProfile(mach_inst, imm8);
    }

    // ---- CPS (Change Processor State) ----
    // Encoding: 10110110 011 im[4] (0)(0) F[1] I[0]
    //   bits[15:5] = 0b10110110011
    // gem5 A-profile: formats/data.isa:1225-1229
    // DDI0403E A7.7.17 (CPS)
    if (bits(inst, 15, 5) == 0x5B3) {  // 0b10110110011
        const bool disable = bits(inst, 4);
        // DDI0403E A7.7.17, Thumb-16 T1 encoding:
        //   bit[1] = I (affects PRIMASK)
        //   bit[0] = F (affects FAULTMASK)
        // Same layout as A-profile (data.isa:1227 uses bits[2:0] = A,I,F)
        const bool affectI = bits(inst, 1);
        const bool affectF = bits(inst, 0);
        return new CpsMProfile(mach_inst, disable, affectI, affectF);
    }

    // ---- SETEND ----
    // Encoding: 10110110 010 E[3] (0000)
    //   bits[15:5] = 0b10110110010
    // M-profile has no SETEND (always LE).  DDI0403E: UNDEFINED.
    // gem5 A-profile: formats/data.isa:1223-1224
    if (bits(inst, 15, 5) == 0x5B2) {  // 0b10110110010
        return new MProfileUndefined(mach_inst, "SETEND");
    }

    // ---- BKPT #0xAB (Semihosting) ----
    // Encoding: 1011 1110 imm8
    //   bits[15:8] = 0xBE, imm8 = 0xAB → inst = 0xBEAB
    // Per the ARM semihosting spec, M-profile uses BKPT #0xAB as the
    // semihosting trigger (not SVC #0xAB which is used by A/R-profile).
    // ABI: R0 = operation code, R1 = parameter block pointer.
    if (bits(inst, 15, 0) == 0xBEAB) {
        return new BkptSemiMProfile(mach_inst);
    }

    // ---- WFI ----
    // Encoding: 10111111 0011 0000
    //   inst = 0xBF30
    // gem5 A-profile: formats/data.isa (Thumb16Misc, hint group)
    // DDI0403E A7.7.184 (WFI)
    if (bits(inst, 15, 0) == 0xBF30) {
        return new WfiMProfile(mach_inst);
    }

    // ---- WFE ----
    // Encoding: 10111111 0010 0000
    //   inst = 0xBF20
    // DDI0403E A7.7.183 (WFE)
    if (bits(inst, 15, 0) == 0xBF20) {
        return new WfeMProfile(mach_inst);
    }

    // ---- PUSH {reglist} / PUSH {reglist, LR} ----
    // Encoding T1: 1011 010 M LLLLLLLL
    //   bits[15:9] = 0b1011010 (0x5A)
    //   bit[8] = M (1 = include LR)
    //   bits[7:0] = register_list (r0-r7)
    // DDI0403E A7.7.101 (PUSH)
    if (bits(inst, 15, 9) == 0x5A) {  // 0b1011010
        uint32_t reglist = bits(inst, 7, 0);
        if (bits(inst, 8))
            reglist |= (1 << int_reg::Lr);
        return new PushPopMProfile("push", mach_inst, false, reglist);
    }

    // ---- POP {reglist} / POP {reglist, PC} ----
    // Encoding T1: 1011 110 P LLLLLLLL
    //   bits[15:9] = 0b1011110 (0x5E)
    //   bit[8] = P (1 = include PC)
    //   bits[7:0] = register_list (r0-r7)
    // DDI0403E A7.7.99 (POP)
    if (bits(inst, 15, 9) == 0x5E) {  // 0b1011110
        uint32_t reglist = bits(inst, 7, 0);
        if (bits(inst, 8))
            reglist |= (1 << int_reg::Pc);
        return new PushPopMProfile("pop", mach_inst, true, reglist);
    }

    // Not an M-profile intercepted 16-bit instruction.
    return nullptr;
}

// =========================================================================
// tryMProfileDecode32 — 32-bit Thumb intercepts
// =========================================================================
//
// Encoding reference: DDI0403E A5.3 (32-bit Thumb instruction encoding)
//
// Instructions intercepted:
//
//   Branches and Misc Ctrl group (HTOPCODE_12_11=0x2, LTOPCODE_15=1):
//     MSR CPSR       — op=0x38/0x39, r=0, bit5=0  → MsrMProfile
//     MSR SPSR       — op=0x38/0x39, r=1           → UsageFault
//     MSR Banked     — op=0x38/0x39, bit5=1        → UsageFault
//     MRS CPSR       — op=0x3e/0x3f, r=0, bit5=0  → MrsMProfile
//     MRS SPSR       — op=0x3e/0x3f, r=1           → UsageFault
//     MRS Banked     — op=0x3e/0x3f, bit5=1        → UsageFault
//     BXJ            — op=0x3c                     → UsageFault
//     ERET           — op=0x3d, imm=0              → UsageFault
//     SUBS PC,LR     — op=0x3d, imm!=0             → UsageFault
//     HVC            — op=0x7e                     → UsageFault
//     SMC            — op=0x7f                     → UsageFault
//
//   SRS/RFE group (HTOPCODE_12_11=0x1, HTOPCODE_10_9=0x0):
//     SRS            — A-profile only              → UsageFault
//     RFE            — A-profile only              → UsageFault
//
//   Coprocessor group:
//     MRC/MCR CP14   — LTCOPROC=0xe                → UsageFault
//     MRC/MCR CP15   — LTCOPROC=0xf                → UsageFault
//     VMRS/VMSR      — LTCOPROC=0xa/0xb            → M-profile FP access
//
//   Misc:
//     SETEND         — [15:5]=10110110010           → UsageFault

StaticInstPtr
MDecoder::tryMProfileDecode32(ExtMachInst mach_inst)
{
    const uint32_t inst = (uint32_t)mach_inst;

    // Thumb-32: hw1 in [31:16], hw2 in [15:0]
    // The ISA description uses these field names (defined in types.hh):
    //   HTOPCODE_N = hw1[N] = combined[N+16]
    //   LTOPCODE = bits[15:0] (second halfword)
    //
    // Key fields (from types.hh Bitfield definitions):
    //   HTOPCODE_12_11 = Bitfield<28,27> = hw1[12:11]
    //   HTOPCODE_10_9  = Bitfield<26,25> = hw1[10:9]
    //   HTOPCODE_8_7   = Bitfield<24,23> = hw1[8:7]
    //   HTOPCODE_6     = Bitfield<22>    = hw1[6]
    //   LTOPCODE_15 = bit[15]
    //   op = bits[26:20]
    //   op1 = bits[14:12]

    const uint32_t htopcode_12_11 = bits(inst, 28, 27);
    const uint32_t htopcode_10_9 = bits(inst, 26, 25);

    // ================================================================
    // SRS / RFE group — A-profile only
    // ================================================================
    // Thumb-32: HTOPCODE_12_11=0x1, HTOPCODE_10_9=0x0,
    //           HTOPCODE_6=0, HTOPCODE_8_7={0x0, 0x3}
    // gem5 A-profile: thumb.isa:68-71 (Thumb32SrsRfe)
    // DDI0403D Table A5-9: op1=01, op2=00xx0xx → Load/Store Multiple
    // DDI0403E: SRS and RFE are UNDEFINED on M-profile.
    if (htopcode_12_11 == 0x1 && htopcode_10_9 == 0x0) {
        if (bits(inst, 22) == 0) {  // HTOPCODE_6 = hw1[6]
            uint32_t htop_8_7 = bits(inst, 24, 23); // hw1[8:7]
            if (htop_8_7 == 0x0 || htop_8_7 == 0x3) {
                return new MProfileUndefined(mach_inst, "SRS/RFE");
            }
        }
    }

    // ================================================================
    // Branches and Misc Ctrl group
    // ================================================================
    // HTOPCODE_12_11=0x2, LTOPCODE_15=1, op1[14:12] & 0x5 == 0
    // gem5 A-profile: thumb.isa:122, formats/branch.isa:116-315
    //
    // This is where MRS, MSR, SMC, HVC, ERET, BXJ, SUBS PC LR live.
    if (htopcode_12_11 == 0x2 && bits(inst, 15) == 1) {
        const uint32_t op = bits(inst, 26, 20);
        const uint32_t op1 = bits(inst, 14, 12);

        // Only the op1 & 0x5 == 0 sub-group contains system instrs
        if ((op1 & 0x5) == 0 && (op & 0x38) == 0x38) {

            switch (op) {
              // ---- MSR (register) ----
              // op = 0x38 or 0x39
              // gem5: formats/branch.isa:144-161
              // DDI0403E A7.7.42
              case 0x38:
              case 0x39: {
                const bool r = bits(inst, 20);
                if (bits(inst, 5)) {
                    // Banked register — A-profile only
                    return new MProfileUndefined(mach_inst,
                        "MSR banked register");
                }
                if (r) {
                    // MSR SPSR — doesn't exist on M-profile
                    return new MProfileUndefined(mach_inst, "MSR SPSR");
                }
                // MSR CPSR → M-profile MSR with SYSm encoding.
                // SYSm is bits[7:0] of the second halfword (DDI0403E
                // A7.7.42, encoding T1).  Note: A-profile uses
                // bits[11:8] as byteMask, but M-profile repurposes
                // the same field space for the SYSm register selector.
                const RegIndex rn = (RegIndex)bits(inst, 19, 16);
                const uint8_t sysM = bits(inst, 7, 0);
                return new MsrMProfile(mach_inst, rn, sysM);
              }

              // ---- Hint instructions (0x3a) ----
              // Contains CPS (Thumb-32 encoding), NOP, etc.
              // These are handled by Thumb-16 format for the common
              // encodings.  The Thumb-32 CPS has a different format
              // from the Thumb-16 one.
              // gem5: formats/branch.isa:163-227
              // For now, let fall through (hints are safe).
              case 0x3a:
                break;

              // ---- DMB / DSB / ISB ----
              // op = 0x3b
              // Sub-decoded by bits[7:4]: 4=DSB, 5=DMB, 6=ISB
              // DMB/DSB: use M-profile barrier (no IsSerializeAfter).
              // ISB: let fall through — A-profile IsSquashAfter is correct.
              // DDI0403E A7.7.27 (DMB), A7.7.28 (DSB), A7.7.29 (ISB)
              case 0x3b: {
                const uint32_t barrierOp = bits(inst, 7, 4);
                if (barrierOp == 0x4) {
                    // DSB — drain write buffer, NO pipeline flush
                    return new BarrierMProfile("dsb", mach_inst);
                } else if (barrierOp == 0x5) {
                    // DMB — memory ordering barrier
                    return new BarrierMProfile("dmb", mach_inst);
                }
                // ISB (barrierOp=0x6): fall through to A-profile decoder
                // (IsSquashAfter pipeline flush is correct for ISB)
                break;
              }

              // ---- BXJ ----
              // op = 0x3c
              // gem5: formats/branch.isa:230-232
              // DDI0403E: BXJ is UNDEFINED on M-profile (no Jazelle).
              case 0x3c:
                return new MProfileUndefined(mach_inst, "BXJ");

              // ---- ERET / SUBS PC,LR ----
              // op = 0x3d
              // gem5: formats/branch.isa:236-242
              // DDI0403E: ERET and SUBS PC,LR,#imm are UNDEFINED
              // on M-profile.  M-profile uses EXC_RETURN via BX.
              case 0x3d:
                return new MProfileUndefined(mach_inst,
                    "ERET/SUBS_PC_LR");

              // ---- MRS ----
              // op = 0x3e or 0x3f
              // gem5: formats/branch.isa:244-261
              // DDI0403E A7.7.41
              case 0x3e:
              case 0x3f: {
                const RegIndex rd = (RegIndex)bits(inst, 11, 8);
                const bool r = bits(inst, 20);
                if (bits(inst, 5)) {
                    // Banked register — A-profile only
                    return new MProfileUndefined(mach_inst,
                        "MRS banked register");
                }
                if (r) {
                    // MRS SPSR — doesn't exist on M-profile
                    return new MProfileUndefined(mach_inst, "MRS SPSR");
                }
                // MRS CPSR → M-profile MRS with SYSm encoding
                // SYSm comes from bits[11:8] in the MRS encoding,
                // but for M-profile the full SYSm is bits[7:0] of
                // the second halfword.
                const uint8_t sysM = bits(inst, 7, 0);
                return new MrsMProfile(mach_inst, rd, sysM);
              }

              // ---- HVC ----
              // op = 0x7e
              // gem5: formats/branch.isa:266-267
              // DDI0403E: UNDEFINED on M-profile (no hypervisor).
              case 0x7e:
                return new MProfileUndefined(mach_inst, "HVC");

              // ---- SMC ----
              // op = 0x7f
              // gem5: formats/branch.isa:128
              // DDI0403E: UNDEFINED on M-profile (no TrustZone
              // in ARMv7-M; ARMv8-M uses different mechanism).
              case 0x7f:
                return new MProfileUndefined(mach_inst, "SMC");

              default:
                break;
            }
        }
    }

    // ================================================================
    // Coprocessor instructions — block CP14/CP15
    // ================================================================
    // CP14 (debug) and CP15 (system control) don't exist on M-profile.
    // M-profile uses MMIO to the SCS instead.
    // CP10/CP11 (FPU) are valid if FPU is present.
    //
    // The coprocessor encoding space is in several HTOPCODE_12_11
    // groups.  We check LTCOPROC (bits[11:8] of second halfword)
    // to identify which coprocessor.
    //
    // gem5 A-profile: thumb.isa:87-88 (mcrMrc14, mcrMrc15)
    // DDI0403E: MRC/MCR CP14/CP15 are UNDEFINED on M-profile.
    {
        const uint32_t ltcoproc = bits(inst, 11, 8);
        // Check if this is a coprocessor instruction by looking at
        // the top-level encoding group.  Coprocessor instructions
        // have HTOPCODE_12_11 = 0x1 or 0x3, HTOPCODE_10_9 >= 0x2.
        // DDI0403D Table A5-9:
        //   op1=01, op2=1xxxxxx → Coprocessor (HTOPCODE_10_9 >= 2)
        //   op1=11, op2=1xxxxxx → Coprocessor (HTOPCODE_10_9 >= 2)
        // gem5 A-profile: thumb.isa:78,124,142 (coprocessor decode paths)
        bool is_coproc = false;
        if (htopcode_12_11 == 0x1 || htopcode_12_11 == 0x3) {
            // HTOPCODE_10_9 (already extracted above) determines the group.
            // Only values >= 2 (hw1[10]=1) are coprocessor instructions.
            if (htopcode_10_9 >= 0x2)
                is_coproc = true;
        }
        if (is_coproc) {
            if (ltcoproc == 0xe) {
                // CP14 — debug coprocessor, A-profile only
                return new MProfileUndefined(mach_inst, "MRC/MCR CP14");
            }
            if (ltcoproc == 0xf) {
                // CP15 — system control, A-profile only
                return new MProfileUndefined(mach_inst, "MRC/MCR CP15");
            }
            // CP10/CP11 (0xa, 0xb) = FPU
            if (ltcoproc == 0xa || ltcoproc == 0xb) {
                if (!has(ArmExtension::M_PROFILE_FPU_SP)) {
                    return new MProfileUndefined(mach_inst,
                        "VFP instruction without FPU in release");
                }
                StaticInstPtr fpInst = decodeMProfileVfp(mach_inst);
                if (fpInst)
                    return fpInst;
                // Unrecognized VFP encoding — fall through to
                // ISA-generated decoder as a safety net.
            }
        }
    }

    // ================================================================
    // LDREX / LDREXB / LDREXH — exclusive load
    // ================================================================
    // The ISA-generated LDREX instruction classes call
    // ArmISA::ISA::getSelfDebug() which static_cast<ISA*>(getIsaPtr()).
    // On M-profile the ISA is MISA (not a subclass of ISA), so the
    // cast is undefined behavior.  Intercept here and return
    // M-profile-specific LDREX classes that skip the SelfDebug call.
    //
    // LDREX  T1: inst[31:20]=0xE85
    // LDREXB T1: inst[31:20]=0xE8D, inst[7:4]=0x4
    // LDREXH T1: inst[31:20]=0xE8D, inst[7:4]=0x5
    {
        const uint32_t op_high = bits(inst, 31, 20);
        if (op_high == 0xE85) {
            // LDREX Rt, [Rn, #imm]
            const RegIndex rt = (RegIndex)bits(inst, 15, 12);
            const RegIndex rn = (RegIndex)bits(inst, 19, 16);
            const uint32_t imm8 = bits(inst, 7, 0) << 2;
            return new LdrexMProfile(mach_inst, rt, rn, imm8, 4);
        }
        if (op_high == 0xE8D) {
            const uint32_t op_low = bits(inst, 7, 4);
            if (op_low == 0x4) {
                // LDREXB Rt, [Rn]
                const RegIndex rt = (RegIndex)bits(inst, 15, 12);
                const RegIndex rn = (RegIndex)bits(inst, 19, 16);
                return new LdrexMProfile(mach_inst, rt, rn, 0, 1);
            }
            if (op_low == 0x5) {
                // LDREXH Rt, [Rn]
                const RegIndex rt = (RegIndex)bits(inst, 15, 12);
                const RegIndex rn = (RegIndex)bits(inst, 19, 16);
                return new LdrexMProfile(mach_inst, rt, rn, 0, 2);
            }
        }
    }

    // ---- 32-bit PUSH (STMDB SP!, {reglist}) ----
    // Encoding T1: 1110 1001 0010 1101 0M0L LLLL LLLL LLLL
    //   bits[31:16] = 0xE92D
    //   bit[14] = M (include LR)
    //   bits[12:0] = register_list (r0-r12)
    // DDI0403E A7.7.101 (PUSH), encoding T2
    if (bits(inst, 31, 16) == 0xE92D) {
        uint32_t reglist = bits(inst, 12, 0);
        if (bits(inst, 14))  // M bit = include LR
            reglist |= (1 << int_reg::Lr);
        return new PushPopMProfile("push.w", mach_inst, false, reglist);
    }

    // ---- 32-bit POP (LDMIA SP!, {reglist}) ----
    // Encoding T2: 1110 1000 1011 1101 PM0L LLLL LLLL LLLL
    //   bits[31:16] = 0xE8BD
    //   bit[15] = P (include PC)
    //   bit[14] = M (include LR)
    //   bits[12:0] = register_list (r0-r12)
    // DDI0403E A7.7.99 (POP), encoding T2
    if (bits(inst, 31, 16) == 0xE8BD) {
        uint32_t reglist = bits(inst, 12, 0);
        if (bits(inst, 14))  // M bit = include LR
            reglist |= (1 << int_reg::Lr);
        if (bits(inst, 15))  // P bit = include PC
            reglist |= (1 << int_reg::Pc);
        return new PushPopMProfile("pop.w", mach_inst, true, reglist);
    }

    // Not an M-profile intercepted 32-bit instruction.
    return nullptr;
}

// =========================================================================
// decodeMProfileVfp — Decode VFP (CP10/CP11) for M-profile
// =========================================================================
//
// VFP data-processing encoding (DDI0403E A7.5, Table A7-17):
//   inst[23:20] = opc1, inst[19:16] = opc2, inst[7:6] = opc3
//   inst[8] = sz (0=single, 1=double)
//
// Single-precision register extraction:
//   Sd = inst[22] | (inst[15:12] << 1)     (D:Vd)
//   Sn = inst[7]  | (inst[19:16] << 1)     (N:Vn)
//   Sm = inst[5]  | (inst[3:0] << 1)       (M:Vm)
//
// VFP register transfer (VMOV core<->VFP, VMRS, VMSR):
//   inst[20] = L (0=to VFP, 1=from VFP)
//   inst[15:12] = Rt (core register)
//   Sn for single-precision = N:Vn

StaticInstPtr
MDecoder::decodeMProfileVfp(ExtMachInst mach_inst)
{
    const uint32_t inst = (uint32_t)mach_inst;

    // Single-precision register extraction helpers.
    auto vd = [&]() -> RegIndex {
        return (RegIndex)(bits(inst, 22) | (bits(inst, 15, 12) << 1));
    };
    auto vn = [&]() -> RegIndex {
        return (RegIndex)(bits(inst, 7) | (bits(inst, 19, 16) << 1));
    };
    auto vm = [&]() -> RegIndex {
        return (RegIndex)(bits(inst, 5) | (bits(inst, 3, 0) << 1));
    };

    const bool single = (bits(inst, 8) == 0);
    const uint32_t opc1 = bits(inst, 23, 20);
    const uint32_t opc2 = bits(inst, 19, 16);
    const uint32_t opc3 = bits(inst, 7, 6);
    const bool bit4 = bits(inst, 4);

    // ---- VFP register transfer (VMOV core<->single, VMRS, VMSR) ----
    // Encoding: opc1[3:1]=0b111, bit4=1 → coprocessor register transfer
    // DDI0403E A7-272: opc1=0b1110, bit4=1 → VMOV (core to/from single)
    //                  opc1=0b1111, bit4=1 → VMRS/VMSR (FPSCR access)
    if (bit4 && bits(opc1, 3, 1) == 0x7) {
        const bool L = bits(inst, 20);  // 0=to VFP, 1=from VFP
        const RegIndex rt = (RegIndex)bits(inst, 15, 12);

        if (bits(opc1, 0) == 0) {
            // VMOV Sn, Rt  /  VMOV Rt, Sn
            if (L) {
                return new MFpMovSToCore(mach_inst, rt, vn());
            } else {
                return new MFpMovCoreToS(mach_inst, vn(), rt);
            }
        } else {
            // VMRS Rt, FPSCR  /  VMSR FPSCR, Rt
            if (L) {
                return new MFpMrs(mach_inst, rt);
            } else {
                return new MFpMsr(mach_inst, rt);
            }
        }
    }

    // ---- VLDR / VSTR (FP load/store) ----
    // Encoding: 1110 110x UDL1 Rn Vd 101s imm8
    //   bit[24]=1, bit[21]=0 distinguishes VLDR/VSTR from VLDM/VSTM.
    //   bit[20]=L (1=load, 0=store), bit[8]=sz (0=single, 1=double)
    // DDI0403E A7.7.236 (VLDR), A7.7.255 (VSTR)
    // Valid for both single (sz=0) and double-word (sz=1) on FPv4-SP.
    // "supports doubleword data transfer instructions" [DDI0403 A6.3]
    if (!bit4 && bits(inst, 24) == 1 && bits(inst, 21) == 0) {
        const bool isLoad = bits(inst, 20);      // L bit
        const bool U = bits(inst, 23);            // add/sub offset
        const uint32_t imm8 = bits(inst, 7, 0);
        const int32_t offset = imm8 << 2;         // imm8 x 4 bytes
        const RegIndex base = (RegIndex)bits(inst, 19, 16);

        if (single) {
            // VLDR.32 / VSTR.32: Sd = Vd:D
            RegIndex sd = (RegIndex)(bits(inst, 15, 12) << 1
                                     | bits(inst, 22));
            if (isLoad)
                return new MFpLdrS(mach_inst, sd, base, offset, U);
            else
                return new MFpStrS(mach_inst, sd, base, offset, U);
        } else {
            // VLDR.64 / VSTR.64: Dd = D:Vd
            RegIndex dd = (RegIndex)(bits(inst, 22) << 4
                                     | bits(inst, 15, 12));
            if (dd > 15)
                return new MProfileUndefined(mach_inst,
                    "VLDR/VSTR D16+ not available on ARMv7-M");
            if (isLoad)
                return new MFpLdrD(mach_inst, dd, base, offset, U);
            else
                return new MFpStrD(mach_inst, dd, base, offset, U);
        }
    }

    // ---- VFP data processing ----
    // Encoding: bit4=0, coproc=0xa/0xb
    // DDI0403E A7-242, Table A7-17
    if (!bit4) {
        // Double-precision check:
        // - Load/store (bit25=0): VLDR/VSTR/VLDM/VSTM/VPUSH/VPOP for
        //   d-registers are valid on FPv4-SP [DDI0403 A6.3].
        //   VLDR/VSTR are intercepted above; others fall through to
        //   the ISA-generated decoder.
        // - Data-processing (bit25=1): VADD.F64, VMUL.F64, etc. require
        //   M_PROFILE_FPU_DP.
        if (!single) {
            bool is_load_store = (bits(inst, 25) == 0);
            if (is_load_store)
                return nullptr;  // allow — fall through to ISA decoder
            if (!has(ArmExtension::M_PROFILE_FPU_DP))
                return new MProfileUndefined(mach_inst,
                    "VFP double-precision arithmetic without FPU_DP");
            return nullptr;  // fall through for DP-capable processors
        }

        switch (opc1 & 0xb /* mask to match A-profile table */) {
          case 0x0:
            // VMLA.F32 / VMLS.F32
            // TODO: implement MFpMacS for multiply-accumulate
            return nullptr;  // fall through for now

          case 0x1:
            // VNMLA.F32 / VNMLS.F32
            return nullptr;

          case 0x2:
            // VMUL.F32 / VNMUL.F32
            if ((opc3 & 0x1) == 0) {
                return new MFpBinS("vmul.f32", mach_inst,
                    SimdFloatMultOp, vd(), vn(), vm(), mFpMul, "vmul");
            } else {
                // VNMUL: negate the result of multiply
                // TODO: implement properly
                return nullptr;
            }

          case 0x3:
            // VADD.F32 / VSUB.F32
            if ((opc3 & 0x1) == 0) {
                return new MFpBinS("vadd.f32", mach_inst,
                    SimdFloatAddOp, vd(), vn(), vm(), mFpAdd, "vadd");
            } else {
                return new MFpBinS("vsub.f32", mach_inst,
                    SimdFloatAddOp, vd(), vn(), vm(), mFpSub, "vsub");
            }

          case 0x8:
            // VDIV.F32
            if ((opc3 & 0x1) == 0) {
                return new MFpBinS("vdiv.f32", mach_inst,
                    SimdFloatDivOp, vd(), vn(), vm(), mFpDiv, "vdiv");
            }
            break;

          case 0x9:
            // VFNMA.F32 / VFNMS.F32
            return nullptr;

          case 0xa:
            // VFMA.F32 / VFMS.F32
            return nullptr;

          case 0xb:
            // VMOV imm / VMOV reg / VNEG / VABS / VCMP / VSQRT / VCVT
            if ((opc3 & 0x1) == 0) {
                // VMOV.F32 Sd, #imm
                const uint32_t baseImm =
                    bits(inst, 3, 0) | (bits(inst, 19, 16) << 4);
                uint32_t imm = vfp_modified_imm(baseImm, FpDataType::Fp32);
                return new MFpMovImmS(mach_inst, vd(), imm);
            }
            // opc3[0]=1: sub-decode on opc2
            switch (opc2) {
              case 0x0:
                if (opc3 == 1) {
                    // VMOV.F32 Sd, Sm (register copy)
                    return new MFpMovRegS(mach_inst, vd(), vm());
                } else {
                    // VABS.F32
                    // TODO: implement MFpUnaryS with fabsf
                    return nullptr;
                }
              case 0x1:
                if (opc3 == 1) {
                    // VNEG.F32
                    // TODO: implement MFpUnaryS with negation
                    return nullptr;
                } else {
                    // VSQRT.F32
                    // TODO: implement — uses SimdFloatSqrtOp
                    return nullptr;
                }
              case 0x4:
              case 0x5: {
                // VCMP.F32 / VCMPE.F32
                const bool withExc = bits(opc2, 0);
                const bool withZero = (opc3 == 3);
                return new MFpCmpS(mach_inst, vd(), vm(),
                                   withExc, withZero);
              }
              default:
                // VCVT and other conversions — fall through for now
                return nullptr;
            }
            break;

          default:
            break;
        }
    }

    // Not recognized — fall through to ISA-generated decoder.
    return nullptr;
}

// =========================================================================
// decodeThumbFallthrough — cached decode via MDecoder::decodeInst
// =========================================================================

StaticInstPtr
MDecoder::decodeThumbFallthrough(ExtMachInst mach_inst, Addr addr)
{
    uint64_t key = (uint64_t)mach_inst;
    auto it = instCache.find(key);
    if (it != instCache.end()) {
        return it->second;
    }

    StaticInstPtr si = decodeInst(mach_inst);

    DPRINTF(Decode, "MDecoder: fallthrough decoded %s: %#x\n",
            si->getName(), (uint64_t)mach_inst);

    instCache[key] = si;
    return si;
}

} // namespace ArmISA
} // namespace gem5
