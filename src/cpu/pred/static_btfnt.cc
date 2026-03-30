/*
 * Copyright (c) 2025 The Regents of The University of California
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

#include "cpu/pred/static_btfnt.hh"

#include "base/trace.hh"
#include "debug/Branch.hh"

namespace gem5
{

namespace branch_prediction
{

StaticBTFNT::StaticBTFNT(const StaticBTFNTParams &params)
    : ConditionalPredictor(params)
{
}

Prediction
StaticBTFNT::lookup(ThreadID tid, Addr pc, void *&bp_history)
{
    bp_history = nullptr;

    auto it = directionCache.find(pc);
    if (it != directionCache.end()) {
        /* Known branch — target was computed at decode on a previous
         * encounter.  Predict taken regardless of direction, modeling
         * the Cortex-M4's early address speculation where the branch
         * target is available at decode [DDI0439D §3.3.1]. */
        DPRINTF(Branch, "StaticBTFNT: PC %#x known → taken\n", pc);
        return predictWithDefaultLatency(true);
    }

    /* First encounter — target not yet computed.  Predict not-taken.
     * Execute will resolve and populate the cache + BTB for next time. */
    DPRINTF(Branch, "StaticBTFNT: PC %#x unknown → not-taken\n", pc);
    return predictWithDefaultLatency(false);
}

void
StaticBTFNT::branchPlaceholder(ThreadID tid, Addr pc,
                                bool uncond, void *&bp_history)
{
    /* No speculative state to allocate */
}

void
StaticBTFNT::updateHistories(ThreadID tid, Addr pc, bool uncond, bool taken,
                              Addr target, const StaticInstPtr &inst,
                              void *&bp_history)
{
    /* Record that we've seen this branch PC.  The target has been
     * computed (from the instruction encoding at decode), so subsequent
     * lookups will predict taken — modeling the Cortex-M4's early
     * address speculation [DDI0439D §3.3.1]. */
    directionCache[pc] = true;
}

void
StaticBTFNT::update(ThreadID tid, Addr pc, bool taken, void *&bp_history,
                     bool squashed, const StaticInstPtr &inst, Addr target)
{
    assert(bp_history == NULL);

    /* Ensure the cache records this branch PC as known. */
    directionCache[pc] = true;
}

} // namespace branch_prediction
} // namespace gem5
