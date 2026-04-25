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

#ifndef __CPU_SIMPLE_3_CYCLE_IN_ORDER_CPU_STATS_HH__
#define __CPU_SIMPLE_3_CYCLE_IN_ORDER_CPU_STATS_HH__

#include "base/statistics.hh"
#include "base/stats/group.hh"

namespace gem5
{

namespace statistics { class Group; }

namespace simple3
{

/**
 * Per-CPU statistics group.  Counters are flat; the gem5 stats
 * machinery is wrapped via statistics::Group so they show up under
 * "system.cpu" in stats.txt.
 */
struct Simple3Stats : public statistics::Group
{
    explicit Simple3Stats(statistics::Group *parent);

    statistics::Scalar instsCommitted;
    statistics::Scalar mispredicts;
    statistics::Scalar requestsIssued;
    statistics::Scalar responsesReceived;
    statistics::Scalar staleResponsesDropped;
    statistics::Scalar flushBubbleCycles;
    statistics::Scalar flashHits;
    statistics::Scalar flashMisses;
    statistics::Scalar dcacheRequestsIssued;
    statistics::Scalar dcacheResponsesReceived;
    statistics::Scalar memStallCycles;
};

} // namespace simple3
} // namespace gem5

#endif // __CPU_SIMPLE_3_CYCLE_IN_ORDER_CPU_STATS_HH__
