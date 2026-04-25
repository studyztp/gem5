/*
 * Copyright (c) 2026 Zhantong Qiu, University of California, Davis
 * and Cornell University
 * All rights reserved.
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

#ifndef __CPU_SIMPLE_3_CYCLE_IN_ORDER_CPU_TYPES_HH__
#define __CPU_SIMPLE_3_CYCLE_IN_ORDER_CPU_TYPES_HH__

#include <cstdint>
#include <memory>

#include "arch/generic/pcstate.hh"
#include "base/types.hh"
#include "cpu/static_inst.hh"

namespace gem5
{
namespace simple3
{

/**
 * A single 32-bit fetch word as it travels from the icache into the
 * PFU FIFO.  The PFU deals in fetch words, not instructions — this
 * keeps the F stage ISA-agnostic.  The D stage feeds the bytes to
 * the generic InstDecoder to produce StaticInstPtr values.
 */
struct FetchWord
{
    Addr addr;       ///< Word-aligned address of this fetch.
    uint32_t data;   ///< The 32-bit fetch payload.
    uint32_t streamId; ///< AHB stream tag the request was issued with.
};

/**
 * An instruction that has been decoded and is waiting in a pipeline
 * slot (D or E).  The static-prediction fields are filled in at the
 * D stage and consumed at the E stage to detect mispredicts.
 */
struct DecodedSlotEntry
{
    Addr addr;                 ///< PC this instruction lives at.
    StaticInstPtr staticInst;  ///< Decoded instruction (generic).

    /** Snapshot of the post-decode PCStateBase (pc=addr, npc=addr+size,
     *  ISA-specific bits like Thumb mode preserved).  Execute restores
     *  this onto the SimpleThread before invoking
     *  staticInst->execute() so that ALU and branch instructions read
     *  the correct PC.  Stored as shared_ptr so DecodedSlotEntry stays
     *  copyable for std::optional and acceptFromDecode(). */
    std::shared_ptr<PCStateBase> pcStateAtDecode;

    /** True iff Pipeline's E→D forwarding step has already pre-run
     *  this branch's execute() at D-time and resolved its outcome.
     *  When set, Execute treats the inst as a no-op pass-through
     *  (no execute(), no advancePC) and just commits the pre-resolved
     *  next-PC.  This models the Cortex-M4 early-decode branch
     *  resolution mechanism described in stm32g4_no_art.py. */
    bool earlyResolved = false;

    /** The next PC the early-resolve step computed.  Only meaningful
     *  when `earlyResolved` is true. */
    Addr resolvedNpc = 0;
};

/**
 * Result of the E stage for one cycle.  Populated by Execute and
 * consumed by the Pipeline::evaluate() mispredict path.
 */
struct MispredictInfo
{
    bool happened;   ///< True iff the branch's actual direction
                     ///<   disagreed with the static prediction.
    Addr actualPc;   ///< The PC we should now fetch from.
};

/**
 * Emitted by the D stage when it decodes a backward conditional
 * branch; tells the PFU to redirect its fetch pointer.
 */
struct BackwardRedirect
{
    Addr targetPc;
};

} // namespace simple3
} // namespace gem5

#endif // __CPU_SIMPLE_3_CYCLE_IN_ORDER_CPU_TYPES_HH__
