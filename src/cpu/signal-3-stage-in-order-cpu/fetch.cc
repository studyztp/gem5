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

#include <sstream>

#include "cpu/signal-3-stage-in-order-cpu/pipeline.hh"
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
    _eRedirectPendingThisCycle.reset();
    _eRedirectPendingNextCycle.reset();
    _dRedirectPendingThisCycle.reset();
    _dRedirectPendingNextCycle.reset();
    _redirectAppliedThisCycle = false;
    _streamSquashedThisCycle = false;
    DPRINTF(Signal3CPUFetch, "resetTo entry=%#x fetchPc=%#x\n",
            entryPc, _fetchPc);
}

void
Fetch::beginCycle()
{
    // Advance the redirect pipeline register: any redirect sampled
    // last cycle now becomes "this cycle's" pending, ready for
    // settle() to apply.  E-redirects take priority over D's (the
    // architectural-truth path wins over the speculative D-resolve).
    _eRedirectPendingThisCycle = _eRedirectPendingNextCycle;
    _dRedirectPendingThisCycle = _dRedirectPendingNextCycle;
    _eRedirectPendingNextCycle.reset();
    _dRedirectPendingNextCycle.reset();
    _redirectAppliedThisCycle = false;
    _streamSquashedThisCycle = false;
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
        // Insert in-order by address.  Responses can arrive out of
        // order from the memory side: with iiswc's PipelinedSimpleMemory
        // an 8-byte buffer hit returns in 0 cy while a buffer miss
        // takes WS+1 cy, so a fetch issued LATER may complete EARLIER
        // when the two share an 8-byte block.  D consumes the FIFO
        // head and assumes its addr matches _nextInstrAddrToDeliver,
        // so we maintain address-order here.  Fetches within a single
        // stream are always issued in increasing-addr order, so a
        // simple linear scan to find the right slot suffices.  Stale
        // streamId responses are already filtered at the port (see
        // PipelinedSimpleMemory's streamId-flush retract path), so
        // every word that reaches us is in the current stream.
        auto it = fifo.begin();
        while (it != fifo.end() && it->addr < w.addr) ++it;
        // Skip if we already have this word (duplicate piggyback).
        if (it != fifo.end() && it->addr == w.addr) {
            DPRINTF(Signal3CPUFetch,
                    "drop duplicate staged word %#x\n", w.addr);
            continue;
        }
        fifo.insert(it, w);
        DPRINTF(Signal3CPUFetch,
                "delivered word %#x data=%#x to FIFO (size=%lu)\n",
                w.addr, w.data, fifo.size());
        _cpu.pipeline().lineTraceEvent("F.asyncResp");
        _cpu.signalStats().wordsDelivered++;
    }
    currentCycleStaging.clear();
}

bool
Fetch::shouldIssueFetch() const
{
    // Don't issue if a redirect arrived this cycle but the new PC
    // hasn't been latched into _fetchPc yet (PC mux output is in
    // transition through its register stage).  Without this gate
    // F would issue a fetch from the wrong-path _fetchPc with a
    // freshly-bumped streamId, putting wrong-path traffic into
    // memory under the new stream tag.
    if (_eRedirectPendingNextCycle.has_value()
            || _dRedirectPendingNextCycle.has_value())
        return false;

    // The only other gate is the FIFO capacity: don't oversubscribe.
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
    const uint32_t streamId = _cpu.currentStreamId();
    RequestPtr req = std::make_shared<Request>(
        _fetchPc, size, flags, _cpu.instRequestorId());
    req->setStreamId(streamId);
    PacketPtr pkt = new Packet(req, MemCmd::ReadReq);
    pkt->allocate();
    DPRINTF(Signal3CPUFetch,
            "queue fetch addr=%#x streamId=%u "
            "(fifo=%lu in_flight=%u pending=%lu)\n",
            _fetchPc, streamId,
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
Fetch::squashStream()
{
    // Combinational squash of F's wrong-path stream state in
    // response to a redirect signal arriving this cycle.  The
    // PC mux output is registered, so _fetchPc / _wordFifoMinAddr
    // are NOT updated here — applyPcUpdate() handles that at the
    // next cycle's edge.
    DPRINTF(Signal3CPUFetch,
            "squashStream (clear FIFO size=%lu, drop in_flight=%u, "
            "drop pending=%lu)\n",
            fifo.size(), _inFlight, pendingIssueQueue.size());
    _cpu.pipeline().lineTraceEvent("F.squash");
    fifo.clear();
    currentCycleStaging.clear();
    nextCycleStaging.clear();
    // Drop outgoing wrong-path packets we hadn't sent yet.  This
    // is the AHB-issue-side analogue of M4's combinational squash:
    // the redirect signal kills any AHB request that was about to
    // be driven this cycle (TRM §2.2.1 transfer-retraction).
    while (!pendingIssueQueue.empty()) {
        delete pendingIssueQueue.front();
        pendingIssueQueue.pop_front();
    }
    // Stale in-flight responses (already on the bus when redirect
    // arrived) are filtered at recvTimingResp by streamId mismatch.
    // Forget them locally so shouldIssueFetch() can grant new-stream
    // budget once the PC mux update lands next cycle.
    _inFlight = 0;
    _cpu.bumpStreamId();
    publishHead();
}

void
Fetch::applyPcUpdate(Addr target)
{
    // Latched PC mux output: applied at the cycle following the
    // redirect signal's arrival.  squashStream() already cleared
    // the wrong-path stream state on the previous cycle; this just
    // updates the PC so shouldIssueFetch() can begin the new
    // stream's fetches.
    DPRINTF(Signal3CPUFetch,
            "applyPcUpdate target=%#x\n", target);
    _cpu.pipeline().lineTraceEvent("F.pcUpdate");
    _wordFifoMinAddr = target;
    _fetchPc = target & ~Addr(0x3);
    publishHead();
}

void
Fetch::applyRedirect(Addr target)
{
    // Convenience wrapper for callers that want both the squash
    // and the PC update applied immediately (IRQ entry, reset).
    // The pipeline's natural redirect path goes through
    // squashStream() + applyPcUpdate() across two cycles.
    squashStream();
    applyPcUpdate(target);
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

    // 0a. Apply pending PC update from a redirect that arrived
    //     last cycle.  This is the latched output of F's PC mux:
    //     squashStream() ran combinationally last cycle when the
    //     redirect signal arrived; the new PC isn't visible until
    //     this cycle's edge.  Guarded so re-fires within the same
    //     settle pass don't re-apply.  E-side took priority over
    //     D-side at sample time; the slots are therefore exclusive.
    if (!_redirectAppliedThisCycle) {
        if (_eRedirectPendingThisCycle.has_value()) {
            applyPcUpdate(*_eRedirectPendingThisCycle);
            _eRedirectPendingThisCycle.reset();
            _redirectAppliedThisCycle = true;
        } else if (_dRedirectPendingThisCycle.has_value()) {
            applyPcUpdate(*_dRedirectPendingThisCycle);
            _dRedirectPendingThisCycle.reset();
            _redirectAppliedThisCycle = true;
        }
    }

    // 0b. Sample any new redirect produced this cycle: combinationally
    //     squash F's wrong-path stream state and remember the target
    //     for next cycle's PC-mux update.  E-redirects supersede D's
    //     (architectural truth wins over speculation).  Guarded by
    //     _streamSquashedThisCycle so re-fires within the same
    //     settle pass don't re-bump streamId.
    if (!_streamSquashedThisCycle) {
        if (_eRedirectInput && _eRedirectInput->valid()
                && _eRedirectInput->read().has_value()) {
            squashStream();
            _eRedirectPendingNextCycle = *_eRedirectInput->read();
            _dRedirectPendingNextCycle.reset();
            _streamSquashedThisCycle = true;
        } else if (_dRedirectInput && _dRedirectInput->valid()
                                    && _dRedirectInput->read().has_value()) {
            squashStream();
            _dRedirectPendingNextCycle = *_dRedirectInput->read();
            _streamSquashedThisCycle = true;
        }
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
            _cpu.pipeline().lineTraceEvent("F.pop");
            fifo.pop_front();
            _cpu.signalStats().wordsConsumed++;
        }
    }

    // 2. Decide whether to queue a new fetch.  The actual
    // sendTimingReq is deferred to Schedule.
    while (shouldIssueFetch()) {
        PacketPtr pkt = buildFetchPacket();
        _cpu.pipeline().lineTraceEvent("F.queueReq");
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

std::string
Fetch::snapshotString() const
{
    std::ostringstream os;
    os << "pc=0x" << std::hex << _fetchPc
       << " fifo[" << std::dec << fifo.size() << "]={";
    bool first = true;
    for (const auto &w : fifo) {
        if (!first) os << ",";
        os << "0x" << std::hex << w.addr
           << "(s" << std::dec << w.streamId << ")";
        first = false;
    }
    os << "} flt=" << std::dec << _inFlight
       << " pq=" << pendingIssueQueue.size()
       << " min=0x" << std::hex << _wordFifoMinAddr;
    return os.str();
}

} // namespace signal3
} // namespace gem5
