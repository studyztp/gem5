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

#include "cpu/simple-3-cycle-in-order-cpu/stats.hh"

namespace gem5
{
namespace simple3
{

Simple3Stats::Simple3Stats(statistics::Group *parent)
    : statistics::Group(parent),
      ADD_STAT(instsCommitted, statistics::units::Count::get(),
               "Number of instructions committed by E"),
      ADD_STAT(mispredicts, statistics::units::Count::get(),
               "Number of branches whose actual direction disagreed "
               "with the static prediction"),
      ADD_STAT(requestsIssued, statistics::units::Count::get(),
               "Number of icache requests issued by F"),
      ADD_STAT(responsesReceived, statistics::units::Count::get(),
               "Number of icache responses accepted (not stale)"),
      ADD_STAT(staleResponsesDropped, statistics::units::Count::get(),
               "Number of icache responses dropped due to streamId "
               "mismatch"),
      ADD_STAT(flushBubbleCycles, statistics::units::Cycle::get(),
               "Cumulative cycles spent in mispredict flush bubble"),
      ADD_STAT(flashHits, statistics::units::Count::get(),
               "Convenience: per-cycle DPRINTF count of buffer-line "
               "hits (informational)"),
      ADD_STAT(flashMisses, statistics::units::Count::get(),
               "Convenience: per-cycle DPRINTF count of buffer-line "
               "misses (informational)"),
      ADD_STAT(dcacheRequestsIssued, statistics::units::Count::get(),
               "Number of dcache requests sent over dcache_port "
               "(loads + stores, including retried sends)"),
      ADD_STAT(dcacheResponsesReceived, statistics::units::Count::get(),
               "Number of dcache responses accepted by the LSQ"),
      ADD_STAT(memStallCycles, statistics::units::Cycle::get(),
               "Cycles E was stalled waiting for an LSQ response")
{
}

} // namespace simple3
} // namespace gem5
