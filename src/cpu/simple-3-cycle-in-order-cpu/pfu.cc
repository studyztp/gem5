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

#include "cpu/simple-3-cycle-in-order-cpu/pfu.hh"

#include "cpu/simple-3-cycle-in-order-cpu/simple_3cycle_cpu.hh"
#include "debug/Simple3CPUPFU.hh"
#include "mem/request.hh"

namespace gem5
{
namespace simple3
{

PFU::PFU(Simple3CycleCPU &cpu_, unsigned fifoWords)
    : cpu(cpu_),
      fifoCapacityWords(fifoWords),
      _fetchPc(0),
      _wordFifoMinAddr(0)
{
}

void
PFU::reset(Addr entryPc)
{
    fifo.clear();
    responseStaging.clear();
    _fetchPc = entryPc & ~Addr(0x3);
    _wordFifoMinAddr = entryPc;
    _numInFlight = 0;
    _redirectFetchDelay = 0;
}

bool
PFU::fifoHasRoom() const
{
    return fifo.size() < fifoCapacityWords;
}

bool
PFU::shouldIssueFetch() const
{
    // Park during a post-redirect fetch-restart penalty (see
    // taken_branch_redirect_delay param).
    if (_redirectFetchDelay > 0)
        return false;

    // Don't oversubscribe the FIFO: outstanding requests are reserved
    // slots that will land via deliverWord() when they return.
    if (fifo.size() + _numInFlight >= fifoCapacityWords)
        return false;

    // If a halt PC is configured, never fetch past it — keeps the
    // smoke test from generating an unbounded stream of stale
    // responses after archPc reaches haltAddr.
    if (cpu.haltAddr() != 0 && _fetchPc >= cpu.haltAddr())
        return false;

    return true;
}

PacketPtr
PFU::buildFetchPacket(uint32_t streamId, RequestorID id)
{
    const unsigned size = sizeof(uint32_t);
    Request::Flags flags = Request::INST_FETCH;
    if (_nextFetchBypassBuffer) {
        // First fetch after a taken redirect — model the Cortex-M4
        // empirical behaviour where this fetch always pays the full
        // Flash WS latency even on a hit (TAKEN_BRANCH_HCLK bundle
        // in the prototype's verify_model.py).  Setting UNCACHEABLE
        // tells PipelinedSimpleMemory to skip its buffer-hit shortcut.
        flags = flags | Request::UNCACHEABLE;
        _nextFetchBypassBuffer = false;
    }
    RequestPtr req = std::make_shared<Request>(
        _fetchPc, size, flags, id);
    req->setStreamId(streamId);
    PacketPtr pkt = new Packet(req, MemCmd::ReadReq);
    pkt->allocate();
    DPRINTF(Simple3CPUPFU,
            "buildFetchPacket addr=%#x streamId=%u uncacheable=%d\n",
            _fetchPc, streamId, (int)req->isUncacheable());
    _fetchPc += size;
    return pkt;
}

void
PFU::filterFifoAndRedirect(Addr actualPc)
{
    const Addr targetWord = actualPc & ~Addr(0x3);
    std::deque<FetchWord> kept;
    for (const auto &w : fifo) {
        if (w.addr >= targetWord)
            kept.push_back(w);
    }
    fifo = std::move(kept);

    _wordFifoMinAddr = actualPc;
    if (!fifo.empty()) {
        _fetchPc = fifo.back().addr + sizeof(uint32_t);
    } else {
        _fetchPc = targetWord;
    }
}

void
PFU::staticPredictRedirect(Addr targetPc, bool sameLine)
{
    // Two cases per Phase D.5 v3 fix.
    //
    // sameLine = true (forward branch with target in same 8-byte
    // Flash line as fall-through, the only case Pipeline currently
    // marks as cheap):
    //   The speculative fall-through fetch went sequentially past
    //   the branch and may have already pulled the target word into
    //   the FIFO (target is AHEAD of fall-through within the same
    //   line).  Keep entries at addr >= target — the target word is
    //   in there, so no re-fetch is needed (silicon's
    //   MISPREDICT_SAME_LINE = 2 HCLK).  Don't bypass the buffer.
    //
    // sameLine = false (backward, or forward to a different line):
    //   Speculative fall-through entries are wrong-path.  Clear the
    //   FIFO entirely (kept entries below or above the target both
    //   cause mis-decode against the new PC), reset fetchPc, and
    //   arm UNCACHEABLE so the first re-fetch pays full WS latency
    //   (silicon's TAKEN_BRANCH bundle = 10 HCLK).
    //
    // Both paths reset _numInFlight: stale in-flight fetches will be
    // dropped by the recvTimingResp streamId check on arrival, but
    // we shouldn't let them gate shouldIssueFetch() while they
    // drain.  (Phase D.5 v4 — fixed `align` Loop A's 5-cy/iter
    // regression caused by the PFU's halt-far-past-loop speculation
    // building up to capacity.)
    if (sameLine) {
        filterFifoAndRedirect(targetPc);
        _nextFetchBypassBuffer = false;
    } else {
        fifo.clear();
        _wordFifoMinAddr = targetPc;
        _fetchPc = targetPc & ~Addr(0x3);
        _nextFetchBypassBuffer = true;
    }
    _numInFlight = 0;
}

std::optional<FetchWord>
PFU::popFrontOfFifo()
{
    if (fifo.empty())
        return std::nullopt;
    FetchWord w = fifo.front();
    fifo.pop_front();
    return w;
}

void
PFU::deliverWord(const FetchWord &word)
{
    // Word-level filter: drop anything that's wholly below the
    // current minimum delivery address (e.g. retracted speculative
    // fetches from before a redirect).
    if (word.addr + sizeof(uint32_t) <= _wordFifoMinAddr)
        return;
    fifo.push_back(word);
}

void
PFU::drainResponseStaging()
{
    if (responseStaging.empty())
        return;
    for (const auto &w : responseStaging)
        deliverWord(w);
    responseStaging.clear();
}

} // namespace simple3
} // namespace gem5
