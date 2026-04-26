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

#include "cpu/signal-3-stage-in-order-cpu/stats.hh"

namespace gem5
{
namespace signal3
{

SignalCPUStats::SignalCPUStats(statistics::Group *parent)
    : statistics::Group(parent),
      ADD_STAT(requestsIssued, statistics::units::Count::get(),
               "Number of icache fetch requests sent"),
      ADD_STAT(responsesReceived, statistics::units::Count::get(),
               "Number of icache responses received (including drops)"),
      ADD_STAT(staleResponsesDropped, statistics::units::Count::get(),
               "Number of icache responses dropped due to streamId mismatch"),
      ADD_STAT(wordsDelivered, statistics::units::Count::get(),
               "Number of fetch words pushed into the F FIFO"),
      ADD_STAT(wordsConsumed, statistics::units::Count::get(),
               "Number of fetch words popped from the F FIFO"),
      ADD_STAT(settleIterations, statistics::units::Count::get(),
               "Total Settle iterations across all cycles"),
      ADD_STAT(maxSettleIterationsThisCycle,
               statistics::units::Count::get(),
               "Maximum Settle iterations in any single cycle"),
      ADD_STAT(mispredicts, statistics::units::Count::get(),
               "Branch mispredict redirects (placeholder for S4+)"),
      ADD_STAT(instsCommitted, statistics::units::Count::get(),
               "Instructions committed at E"),
      ADD_STAT(dcacheRequestsIssued, statistics::units::Count::get(),
               "Number of dcache requests sent over dcache_port"),
      ADD_STAT(dcacheResponsesReceived, statistics::units::Count::get(),
               "Number of dcache responses accepted by the LSQ"),
      ADD_STAT(memStallCycles, statistics::units::Cycle::get(),
               "Cycles E was stalled waiting for an LSQ response")
{
}

} // namespace signal3
} // namespace gem5
