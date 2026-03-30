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

#ifndef __ARCH_ARM_M_DECODER_HH__
#define __ARCH_ARM_M_DECODER_HH__

/** @file
 * M-profile Thumb decoder.
 *
 * Inherits from InstDecoder (NOT from ArmDecoder/Decoder) because
 * the A-profile Decoder constructor does safe_cast<ISA*> which fails
 * for M-profile MISA (BaseISA subclass, not ISA subclass).
 *
 * This is the same pattern as MISA inheriting from BaseISA instead
 * of ISA: the A-profile classes have too many A-profile assumptions.
 *
 * The Thumb instruction assembly logic (moreBytes, process) is
 * replicated from Decoder — it's ~90 lines and has no A-profile
 * dependencies.  The decode path uses tryMProfileDecode for
 * M-profile-specific instructions and falls through to the
 * ISA-generated Thumb decoder for everything else.
 */

#include <unordered_map>

#include "arch/arm/decoder.hh"
#include "arch/arm/m_system.hh"
#include "arch/arm/types.hh"
#include "enums/DecoderFlavor.hh"
#include "params/ArmMDecoder.hh"

namespace gem5
{

namespace ArmISA
{

class MDecoder : public InstDecoder
{
  public:
    // -- Members accessed by ISA-generated decode tree --
    // The generated decodeInst() reads decoderFlavor to select
    // instruction variants.  decodeBranchExcSys reads dvmEnabled.
    // M-profile defaults: Generic flavor, no DVM.
    enums::DecoderFlavor decoderFlavor;
    bool dvmEnabled;

  protected:
    // -- System pointer for release/extension checking --
    // Used to gate M-profile-specific instructions (e.g., FPU) on the
    // CPU's declared extensions (M_PROFILE_FPU_SP, M_PROFILE_FPU_DP).
    ArmMSystem *mSystem;

    /** Check whether the system's release includes a given extension. */
    bool has(ArmExtension ext) const
    {
        return mSystem && mSystem->has(ext);
    }

    // -- Instruction assembly state (from Decoder) --
    ExtMachInst emi;
    uint32_t data;
    bool bigThumb;
    int offset;
    bool foundIt;
    ITSTATE itBits;

    // -- Decode cache --
    std::unordered_map<uint64_t, StaticInstPtr> instCache;

    // -- EXC_RETURN detection for POP {PC} / LDM {PC} --
    // When a load writes an EXC_RETURN value to PC, the CPU tries
    // to fetch from 0xFFFFFFF_.  moreBytes() detects this address
    // pattern and sets these flags so decode() can handle it.
    bool _pendingExcReturn = false;
    uint32_t _excReturnVal = 0;

    /** Thumb instruction assembly state machine.
     *  Copied from Decoder::process() — no A-profile dependencies. */
    void process();

    /** Advance offset within the fetch data word. */
    void consumeBytes(int numBytes);

  public:
    PARAMS(ArmMDecoder);
    MDecoder(const Params &params);

    void reset() override;
    void moreBytes(const PCStateBase &pc, Addr fetchPC) override;
    StaticInstPtr decode(PCStateBase &pc) override;

    /** ISA-generated decode function.
     *  Generated from the same .isa files as Decoder::decodeInst,
     *  via #define Decoder MDecoder / #include decode-method.cc.inc.
     *  Stateless: only examines machInst bits, no member access. */
    StaticInstPtr decodeInst(ExtMachInst mach_inst);

    /** Fall through to ISA-generated Thumb decoder for non-M-profile
     *  instructions.  Returns cached result or decodes fresh. */
    StaticInstPtr decodeThumbFallthrough(ExtMachInst mach_inst, Addr addr);

  private:
    /** Try to decode as M-profile-specific instruction. */
    StaticInstPtr tryMProfileDecode(ExtMachInst mach_inst);
    StaticInstPtr tryMProfileDecode16(ExtMachInst mach_inst);
    StaticInstPtr tryMProfileDecode32(ExtMachInst mach_inst);

    /** Decode VFP (CP10/CP11) coprocessor instructions for M-profile.
     *  Returns nullptr if the encoding is not recognized (falls through
     *  to ISA-generated decoder). */
    StaticInstPtr decodeMProfileVfp(ExtMachInst mach_inst);
};

} // namespace ArmISA
} // namespace gem5

#endif // __ARCH_ARM_M_DECODER_HH__
