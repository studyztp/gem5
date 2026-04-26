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

#include "cpu/signal-3-stage-in-order-cpu/fetch.hh"

#include "cpu/signal-3-stage-in-order-cpu/signal_cpu.hh"
#include "debug/Signal3CPUFetch.hh"
#include "debug/Signal3CPUMem.hh"

namespace gem5
{
namespace signal3
{

Fetch::Fetch(SignalGraph &g, SignalCPU &cpu, unsigned fifoCapacity)
    : Stage(g, "fetch", /*stageId=*/0),
      f_word_out(g, "f_word_out", /*fromStage=*/this, /*pulse=*/false),
      f_can_accept(g, "f_can_accept", /*fromStage=*/this,
                   /*pulse=*/false),
      _cpu(cpu),
      _fifoCapacity(fifoCapacity)
{
    // Subscribers (D's d_pop_request, redirects from D/E) are wired
    // by Pipeline after all stages exist.
}

void
Fetch::resetTo(Addr entryPc)
{
    fifo.clear();
    currentCycleStaging.clear();
    nextCycleStaging.clear();
    pendingIssueQueue.clear();
    _fetchPc = entryPc & ~Addr(0x3);
    _wordFifoMinAddr = entryPc;
    _inFlight = 0;
    _drainedThisSettle = false;
    DPRINTF(Signal3CPUFetch, "resetTo entry=%#x fetchPc=%#x\n",
            entryPc, _fetchPc);
}

void
Fetch::recvWordNow(const FetchWord &w)
{
    DPRINTF(Signal3CPUMem,
            "recvWordNow addr=%#x data=%#x streamId=%u "
            "(current-cycle staging)\n",
            w.addr, w.data, w.streamId);
    currentCycleStaging.push_back(w);
}

void
Fetch::stageForNextCycle(const FetchWord &w)
{
    DPRINTF(Signal3CPUMem,
            "stageForNextCycle addr=%#x data=%#x streamId=%u\n",
            w.addr, w.data, w.streamId);
    nextCycleStaging.push_back(w);
}

void
Fetch::noteResponseArrived()
{
    if (_inFlight > 0)
        --_inFlight;
}

void
Fetch::absorbStaging()
{
    // Move next-cycle staging (deposited last cycle by recvTimingResp
    // when the response missed our active cycle window) into the
    // current bucket so settle() drains both uniformly.
    for (auto &w : nextCycleStaging)
        currentCycleStaging.push_back(w);
    nextCycleStaging.clear();

    if (currentCycleStaging.empty())
        return;

    for (const auto &w : currentCycleStaging) {
        // R6 word-level filter: drop anything wholly below the
        // current minimum delivery address (e.g. retracted
        // speculative fetches from before a redirect).
        if (w.addr + sizeof(uint32_t) <= _wordFifoMinAddr) {
            DPRINTF(Signal3CPUFetch,
                    "drop staged word %#x (below min %#x)\n",
                    w.addr, _wordFifoMinAddr);
            continue;
        }
        fifo.push_back(w);
        DPRINTF(Signal3CPUFetch,
                "delivered word %#x data=%#x to FIFO (size=%lu)\n",
                w.addr, w.data, fifo.size());
        _cpu.signalStats().wordsDelivered++;
    }
    currentCycleStaging.clear();
}

bool
Fetch::shouldIssueFetch() const
{
    // The only gate is the FIFO capacity: don't oversubscribe.
    // Outstanding requests reserve FIFO slots that get returned via
    // recvTimingResp -> noteResponseArrived.  Halting is the
    // Pipeline's job (haltConditionMet checks archPc), not F's.
    if (fifo.size() + _inFlight + pendingIssueQueue.size()
            >= _fifoCapacity)
        return false;
    return true;
}

PacketPtr
Fetch::buildFetchPacket()
{
    const unsigned size = sizeof(uint32_t);
    Request::Flags flags = Request::INST_FETCH;
    if (_nextFetchBypassBuffer) {
        // S6: post-redirect first fetch — force a miss on the Flash
        // 64-bit current-line buffer.  PipelinedSimpleMemory honours
        // Request::UNCACHEABLE by skipping its buffer-hit shortcut
        // and paying the full WS+1 cycle penalty.
        flags = flags | Request::UNCACHEABLE;
        _nextFetchBypassBuffer = false;
    }
    const uint32_t streamId = _cpu.currentStreamId();
    RequestPtr req = std::make_shared<Request>(
        _fetchPc, size, flags, _cpu.instRequestorId());
    req->setStreamId(streamId);
    PacketPtr pkt = new Packet(req, MemCmd::ReadReq);
    pkt->allocate();
    DPRINTF(Signal3CPUFetch,
            "queue fetch addr=%#x streamId=%u uncacheable=%d "
            "(fifo=%lu in_flight=%u pending=%lu)\n",
            _fetchPc, streamId, (int)req->isUncacheable(),
            fifo.size(), _inFlight, pendingIssueQueue.size());
    _fetchPc += size;
    return pkt;
}

void
Fetch::publishHead()
{
    if (fifo.empty()) {
        f_word_out.write(std::optional<FetchWord>{});
    } else {
        f_word_out.write(std::optional<FetchWord>{fifo.front()});
    }
    f_can_accept.write(fifo.size() < _fifoCapacity);
}

void
Fetch::applyRedirect(Addr target)
{
    DPRINTF(Signal3CPUFetch,
            "applyRedirect target=%#x (clear FIFO size=%lu, "
            "drop in_flight=%u, drop pending=%lu)\n",
            target, fifo.size(), _inFlight,
            pendingIssueQueue.size());
    fifo.clear();
    currentCycleStaging.clear();
    nextCycleStaging.clear();
    // Drop any outgoing wrong-path packets we hadn't yet sent.
    // Without this, scheduleOutgoing would still send them after
    // the redirect, mis-bumping in_flight (and forcing the stream
    // to wait for their responses to clear before the right-path
    // fetches fit in the in-flight budget).
    while (!pendingIssueQueue.empty()) {
        delete pendingIssueQueue.front();
        pendingIssueQueue.pop_front();
    }
    _wordFifoMinAddr = target;
    _fetchPc = target & ~Addr(0x3);
    // Stale in-flight responses will be dropped at recvTimingResp
    // by the streamId mismatch.  Forget them locally so
    // shouldIssueFetch() doesn't gate the new stream's first fetch.
    _inFlight = 0;
    _cpu.bumpStreamId();
    // S6: arm the next outgoing fetch to bypass the Flash 64-bit
    // line buffer (pays full WS latency).  Models silicon's
    // TAKEN_BRANCH bundle.
    _nextFetchBypassBuffer = true;
    publishHead();
}

void
Fetch::settle()
{
    // Drain async-ingress unconditionally — absorbStaging() empties
    // the staging buffers, so re-firing settle() within the same
    // Settle pass is a safe no-op.  This also lets a mid-cycle
    // recvWordNow() (from onAsyncResponse) be picked up by a
    // subsequent re-fire within the same cycle.
    absorbStaging();

    // 0a. Apply E's redirect (highest priority — represents the
    //     architectural truth, all D-side speculation is wrong).
    if (_eRedirectInput && _eRedirectInput->valid()
            && _eRedirectInput->read().has_value()) {
        applyRedirect(*_eRedirectInput->read());
    } else if (_dRedirectInput && _dRedirectInput->valid()
                                && _dRedirectInput->read().has_value()) {
        // 0b. S5: D's same-cycle redirect (early-resolved branch
        //     using forwarded flags from E).  Same applyRedirect
        //     mechanism — clear FIFO and bump streamId.
        applyRedirect(*_dRedirectInput->read());
    }

    // 1. If D popped this cycle, advance the FIFO head.
    const bool popReq = _popRequestInput
                        && _popRequestInput->valid()
                        && _popRequestInput->read();
    if (popReq) {
        if (!fifo.empty()) {
            DPRINTF(Signal3CPUFetch,
                    "pop FIFO head %#x (size %lu->%lu)\n",
                    fifo.front().addr, fifo.size(), fifo.size() - 1);
            fifo.pop_front();
            _cpu.signalStats().wordsConsumed++;
        }
    }

    // 2. Decide whether to queue a new fetch.  The actual
    // sendTimingReq is deferred to Schedule.
    while (shouldIssueFetch()) {
        PacketPtr pkt = buildFetchPacket();
        pendingIssueQueue.push_back(pkt);
    }

    // 3. Publish state to subscribers.
    publishHead();

    DPRINTF(Signal3CPUFetch,
            "settle done: fetchPc=%#x fifo=%lu in_flight=%u pending=%lu\n",
            _fetchPc, fifo.size(), _inFlight, pendingIssueQueue.size());
}

bool
Fetch::tryIssuePendingFetch()
{
    if (pendingIssueQueue.empty())
        return true;
    PacketPtr pkt = pendingIssueQueue.front();
    if (!_cpu.getIcachePort().sendTimingReq(pkt)) {
        DPRINTF(Signal3CPUMem,
                "icache port refused addr=%#x — will retry\n",
                pkt->getAddr());
        return false;
    }
    DPRINTF(Signal3CPUMem,
            "icache sent addr=%#x streamId=%u\n",
            pkt->getAddr(), pkt->req->streamId());
    _cpu.signalStats().requestsIssued++;
    ++_inFlight;
    pendingIssueQueue.pop_front();
    return true;
}

} // namespace signal3
} // namespace gem5
