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

#ifndef __CPU_PRED_STATIC_BTFNT_HH__
#define __CPU_PRED_STATIC_BTFNT_HH__

#include <unordered_map>

#include "base/types.hh"
#include "cpu/pred/branch_type.hh"
#include "cpu/pred/conditional.hh"
#include "params/StaticBTFNT.hh"

namespace gem5
{

namespace branch_prediction
{

/**
 * Static BTFNT (Backward Taken, Forward Not-Taken) branch predictor.
 *
 * Models the Cortex-M4's branch handling where there is no learned
 * predictor.  Backward branches (target < pc) are predicted taken via
 * early address speculation; forward branches are assumed not-taken.
 *
 * A small direction cache records whether each branch PC is backward
 * or forward.  On the first encounter the direction is unknown, so
 * the branch is predicted not-taken; execute corrects if needed and
 * the cache is populated for subsequent encounters.
 *
 * No per-branch history is maintained (bp_history is always NULL).
 */
class StaticBTFNT : public ConditionalPredictor
{
  public:
    StaticBTFNT(const StaticBTFNTParams &params);

    Prediction lookup(ThreadID tid, Addr pc, void *&bp_history) override;

    void branchPlaceholder(ThreadID tid, Addr pc, bool uncond,
                           void *&bp_history) override;

    void updateHistories(ThreadID tid, Addr pc, bool uncond, bool taken,
                         Addr target, const StaticInstPtr &inst,
                         void *&bp_history) override;

    void update(ThreadID tid, Addr pc, bool taken, void *&bp_history,
                bool squashed, const StaticInstPtr &inst,
                Addr target) override;

    void squash(ThreadID tid, void *&bp_history) override
    { assert(bp_history == NULL); }

  private:
    /** Cache mapping branch PC → is_backward (target < pc). */
    std::unordered_map<Addr, bool> directionCache;
};

} // namespace branch_prediction
} // namespace gem5

#endif // __CPU_PRED_STATIC_BTFNT_HH__
