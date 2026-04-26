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

#include "cpu/signal-3-stage-in-order-cpu/signal.hh"

#include <sstream>
#include <utility>

namespace gem5
{
namespace signal3
{

SignalGraph::SignalGraph() = default;

void
SignalGraph::registerStage(Stage *s)
{
    _stages.push_back(s);
}

void
SignalGraph::registerLatchCommit(std::function<void()> commitEdge,
                                 const std::string &nm)
{
    _latchCommits.push_back(std::move(commitEdge));
    _latchNames.push_back(nm);
}

void
SignalGraph::registerSignal(ISignalBase *s)
{
    _signals.push_back(s);
}

void
SignalGraph::beginCycle(Tick startTick, Tick periodTicks)
{
    _cycleStart = startTick;
    _cyclePeriod = periodTicks;
    // Pulse signals (e.g. d_redirect_to_f) must not bleed into the
    // next cycle.  Reset them before the Edge phase samples latches.
    for (ISignalBase *s : _signals)
        s->resetPulse();
    // First Settle pass after Edge runs every stage so they can
    // observe latch outputs.
    for (Stage *s : _stages)
        _dirtyStages.insert(s);
    DPRINTF(Signal3CPUPipeline,
            "beginCycle start=%llu period=%llu seeded=%lu\n",
            (unsigned long long)startTick,
            (unsigned long long)periodTicks,
            _stages.size());
}

void
SignalGraph::edge()
{
    DPRINTF(Signal3CPUPipeline, "edge: committing %lu latches\n",
            _latchCommits.size());
    for (auto &fn : _latchCommits)
        fn();
}

void
SignalGraph::settle()
{
    panic_if(_inSettle,
             "SignalGraph::settle re-entered (mid-Settle response?)");
    _inSettle = true;

    int iter = 0;
    while (!_dirtyStages.empty()) {
        panic_if(iter >= kMaxSettleIters,
                 "Settle non-convergence after %d iterations; "
                 "dirty={%s}", iter,
                 dirtyStagesString().c_str());

        // Snapshot and clear the dirty set; downstream writes during
        // this pass will repopulate it for the next iteration.
        std::set<Stage *, StageOrder> pass;
        pass.swap(_dirtyStages);

        DPRINTF(Signal3CPUPipeline,
                "settle iter=%d firing %lu stage(s)\n",
                iter, pass.size());

        for (Stage *s : pass)
            s->settle();
        ++iter;
    }
    DPRINTF(Signal3CPUPipeline,
            "settle done after %d iter(s)\n", iter);
    _inSettle = false;
}

bool
SignalGraph::isWithinCycle(Tick t) const
{
    return t >= _cycleStart && t < _cycleStart + _cyclePeriod;
}

void
SignalGraph::markDirty(Stage *s)
{
    _dirtyStages.insert(s);
}

std::string
SignalGraph::dirtyStagesString() const
{
    std::ostringstream os;
    bool first = true;
    for (Stage *s : _dirtyStages) {
        if (!first) os << ",";
        os << s->name();
        first = false;
    }
    return os.str();
}

} // namespace signal3
} // namespace gem5
