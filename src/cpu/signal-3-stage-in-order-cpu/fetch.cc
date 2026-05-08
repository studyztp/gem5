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

Fetch::Fetch(SignalGraph &g, SignalCPU &cpu, unsigned fifoCapacity,
             unsigned maxOutstandingFetches)
    : Stage(g, "fetch", /*stageId=*/0),
      f_word_out(g, "f_word_out", /*fromStage=*/this, /*pulse=*/false),
      f_can_accept(g, "f_can_accept", /*fromStage=*/this,
                   /*pulse=*/false),
      _cpu(cpu),
      _fifoCapacity(fifoCapacity),
      _maxOutstandingFetches(maxOutstandingFetches)
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

namespace {
// Detect 16-bit Cortex-M Thumb branch encodings in a halfword.
// Covers the redirecting 16-bit insts that show up in our microbench
// kernels and the harness:
//   - 16-bit B<cond>      `1101 cccc xxxx xxxx`  (cond != 0xE/0xF)
//   - 16-bit B (uncond)   `11100 imm11`          (top 5 bits = 0x1C)
//   - 16-bit BX/BLX       `0100 0111 X 0 mmm 000`
//
// 32-bit Thumb-2 branches (B.W / BL / BLX) are intentionally NOT
// predecoded here.  Distinguishing a 32-bit branch (first hw top5
// = 11110, second hw top1 = 1) from a non-branch 32-bit inst with
// the same first-hw pattern (msr / mrs / etc.) requires a fuller
// decoder than F has available.  Counting them as branches and
// decrementing on every isControl commit at E would mismatch when
// non-control 32-bit insts share the pattern.  Instead we
// underestimate (no predecode for 32-bit branches) and pair the
// decrement at E to 16-bit control insts only — see
// Execute::commitOne.  The microbench kernels use 16-bit branches;
// 32-bit BL/BLX in the harness prologue are bypassed cleanly
// because no predecode increment ever fires for them.
bool
isLikelyBranch16(uint16_t hw)
{
    // 16-bit B<cond> imm8
    if ((hw & 0xF000) == 0xD000) {
        const unsigned cond = (hw >> 8) & 0xF;
        return cond != 0xE && cond != 0xF;
    }
    // 16-bit B (unconditional) imm11
    if ((hw & 0xF800) == 0xE000)
        return true;
    // 16-bit BX/BLX register
    if ((hw & 0xFF00) == 0x4700)
        return true;
    return false;
}

// True if `hw` is the first halfword of a 32-bit Thumb-2 inst.
// Per ARMv7-M ARM A5.1, top 5 bits in {11101, 11110, 11111}.
bool
isThumb2First(uint16_t hw)
{
    const unsigned top5 = (hw >> 11) & 0x1F;
    return top5 == 0x1D || top5 == 0x1E || top5 == 0x1F;
}

// Predecode result: branch flag plus the sticky state for the
// next word's predecode.  The sticky bit tells the next call
// "your lo is the second half of a 32-bit Thumb-2 inst — skip it."
struct PredecodeResult
{
    bool branchSeen;
    bool nextWordLoIsThumb2Tail;
};

// Returns whether the 32-bit fetched word contains a live 16-bit
// branch halfword (`branchSeen`), and whether the FOLLOWING word's
// lo will be the second half of a 32-bit Thumb-2 inst started
// here (`nextWordLoIsThumb2Tail`).
//
// Address filter: `minDeliverAddr` rules out halfwords below D's
// current delivery point — e.g. the first halfword of a fetch
// when the redirect target is mid-word and D will skip lo.
// Without this filter, a flagged branch that never commits leaves
// the F predecode counter stuck > 0 and hangs the CPU.
//
// Halfword classification: 32-bit Thumb-2 insts have a defining
// first-half bit pattern (top5 in {11101, 11110, 11111}).  Their
// second half is instruction *data* — register lists, immediates,
// etc. — which can match 16-bit branch encodings by accident
// (e.g. push.w {r4-r10,lr}'s hi = 0x47F0 matches BX, ubfx.w's tail
// can match Bcond).  We must not predecode such tail bytes as
// 16-bit insts.  Three positions:
//   - lo = first-half-of-32-bit-from-prev-word (sticky in flag)
//   - lo = first-half-of-32-bit (covers the whole word)
//   - lo = 16-bit, hi = first-half-of-32-bit (hi's tail is in next word)
// Each is handled below.  The `isLikelyBranch16` only flags 16-bit
// branches, so 32-bit B.W/BL/BLX are NOT predecoded; F will
// speculate past them, producing a small over-prediction in
// kernels that contain 32-bit branches (none of the microbenches
// here do — they're all 16-bit).
PredecodeResult
predecodeFetchWord(Addr wordAddr, uint32_t data, Addr minDeliverAddr,
                   bool loIsThumb2Tail)
{
    const uint16_t lo = data & 0xFFFF;
    const uint16_t hi = (data >> 16) & 0xFFFF;

    PredecodeResult r{false, false};

    // Case 1: lo is the second half of a 32-bit inst that started
    // in the previous word.  Don't predecode lo — it's tail data.
    // hi may be the start of a fresh inst (16-bit or 32-bit-first).
    if (loIsThumb2Tail) {
        if (isThumb2First(hi)) {
            // hi starts a new 32-bit; its tail is in next word's lo.
            r.nextWordLoIsThumb2Tail = true;
        } else if ((wordAddr + 2) >= minDeliverAddr
                   && isLikelyBranch16(hi)) {
            r.branchSeen = true;
        }
        return r;
    }

    // Case 2: lo is itself the first half of a 32-bit inst.
    // The word holds one 32-bit inst; nothing more to predecode.
    if (isThumb2First(lo)) {
        // Tail (hi) is data of the 32-bit inst, not a new inst.
        // Next word's lo is a fresh inst (no sticky).
        return r;
    }

    // Case 3: lo is a 16-bit inst.
    if (wordAddr >= minDeliverAddr && isLikelyBranch16(lo)) {
        r.branchSeen = true;
        // Continue checking hi — there could be more in this word.
    }

    // hi position: at addr wordAddr+2.  Either 16-bit inst or
    // first half of a 32-bit inst whose tail spans into next word.
    if (isThumb2First(hi)) {
        r.nextWordLoIsThumb2Tail = true;
    } else if ((wordAddr + 2) >= minDeliverAddr
               && isLikelyBranch16(hi)) {
        r.branchSeen = true;
    }

    return r;
}
} // namespace

void
Fetch::noteBranchCommitted()
{
    if (_branchesInFlight > 0) {
        --_branchesInFlight;
        DPRINTF(Signal3CPUFetch,
                "noteBranchCommitted: branchesInFlight now %u\n",
                _branchesInFlight);
    }
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
        // Predecode for branch.  When a fetched word contains a
        // branch halfword, F bumps a counter that gates further
        // speculative issue (shouldIssueFetch).  E clears the
        // counter via noteBranchCommitted() when the branch
        // commits; squashStream resets it on redirect.  Models the
        // Cortex-M4 PFU's behaviour of stopping at unresolved
        // branches (TRM §1.4) — the silicon-measured forward-branch
        // bubble of ~7 cy assumes no wrong-path Flash is in flight
        // when the branch resolves at E.
        const auto pd = predecodeFetchWord(
            w.addr, w.data, _wordFifoMinAddr,
            _nextWordLoIsThumb2Tail);
        _nextWordLoIsThumb2Tail = pd.nextWordLoIsThumb2Tail;
        if (pd.branchSeen) {
            ++_branchesInFlight;
            DPRINTF(Signal3CPUFetch,
                    "predecode: branch in word %#x, branchesInFlight=%u\n",
                    w.addr, _branchesInFlight);
        }
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

    // PFU "stop-at-branch" gate (Cortex-M4 TRM §1.4): the
    // prefetch unit does not speculate past an unresolved
    // branch.  Once F has predecoded a branch in any FIFO word
    // (set in absorbStaging), no further fetches are issued
    // until E commits the branch (noteBranchCommitted) or a
    // redirect clears the count (squashStream).  Without this
    // gate, F speculatively pre-fetches past forward-cond
    // branches, leaves Flash mid-access, and the right-path
    // fetch waits for it to drain — the +25 % over silicon on
    // forward / align / alternating in the calibration data.
    if (_branchesInFlight > 0)
        return false;

    // AHB-Lite back-pressure on the icache port: cap simultaneously-
    // outstanding fetches.  Strict AHB-Lite allows at most 1 transfer
    // in address phase + 1 in data phase = 2 outstanding; the 3rd
    // transfer's address phase has to wait for the 1st transfer's
    // data phase to complete (HREADY=0 stalls the master).  Without
    // this gate F piles 3+ fetches into the Flash port; back-to-back
    // redirects then queue the right-path fetch behind a wrong-path
    // Flash array op, costing ~5 extra cy/iter on forward /
    // alternating.  See branch_error_root_cause_2026-05-07.md.
    //
    // Tunable via SignalCPU.pfu_max_outstanding_fetches (default 2).
    if (_maxOutstandingFetches > 0
            && _inFlight + pendingIssueQueue.size()
                   >= _maxOutstandingFetches)
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
    // All predecoded branches are resolved by the redirect (the
    // taken case implies the branch's outcome was determined; any
    // following branches in flight were on the wrong path and
    // are now gone with the squashed FIFO).
    _branchesInFlight = 0;
    // Redirect target starts a fresh decode flow — clear the
    // sticky 32-bit-tail bit (in-flight wrong-path 32-bit insts
    // are squashed along with the FIFO).
    _nextWordLoIsThumb2Tail = false;
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
    // Drop any pending D/E redirect from a prior cycle's settle —
    // otherwise the next settle pass would apply the stale redirect
    // and overwrite this immediate redirect's PC.  Hits when an
    // interrupt fires the cycle after D resolves a cond branch via
    // forwarded flags: D's d_redirect_to_f is sampled into the
    // pending register at end of the prior cycle, then the IRQ
    // arrives at the next cycle's edge and calls applyRedirect()
    // before settle; without this clear, the IRQ's PC is then
    // clobbered by the still-pending d_redirect_to_f target.
    _eRedirectPendingThisCycle.reset();
    _dRedirectPendingThisCycle.reset();
    _redirectAppliedThisCycle = true;
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
