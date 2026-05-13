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

#ifndef __CPU_SIGNAL_3_STAGE_IN_ORDER_CPU_FETCH_HH__
#define __CPU_SIGNAL_3_STAGE_IN_ORDER_CPU_FETCH_HH__

#include <deque>
#include <optional>
#include <vector>

#include "base/types.hh"
#include "cpu/signal-3-stage-in-order-cpu/signal.hh"
#include "cpu/signal-3-stage-in-order-cpu/types.hh"
#include "mem/packet.hh"
#include "mem/request.hh"

namespace gem5
{
namespace signal3
{

class SignalCPU;

/**
 * Fetch (F) stage — prefetch unit + word FIFO.
 *
 * Outputs (Signals into the rest of the pipeline):
 *   f_word_out      head of the FIFO; std::nullopt iff FIFO empty.
 *   f_can_accept    informational; true iff FIFO has room.
 *
 * Inputs (Signals subscribed-to):
 *   d_pop_request   when true during a Settle pass, F pops its head.
 *   (S4+)  d_redirect_to_f / e_redirect_to_f are added later phases.
 *
 * Latched state across cycles:
 *   fetchPc        next sequential fetch PC.
 *   inFlight       outstanding icache requests.
 *
 * The FIFO is a plain std::deque held as a member (not a Latch<T>) —
 * Latch<deque<T>> would copy O(N) bytes per Edge.  The FIFO is
 * mutated only during Settle and its head is mirrored to f_word_out
 * after every mutation, so downstream subscribers see consistent
 * state.
 *
 * Outgoing icache packets are queued into pendingIssueQueue during
 * settle() and flushed by Pipeline::scheduleOutgoing() at the end
 * of evaluate() — this is the Schedule phase from DESIGN.md §2.
 */
class Fetch : public Stage
{
  public:
    Fetch(SignalGraph &g, SignalCPU &cpu, unsigned fifoCapacity,
          unsigned maxOutstandingFetches);

    void settle() override;

    /* ---- Per-cycle hook (called from Pipeline::evaluate) ---- */
    void beginCycle();

    /* ---- Reset/seed (called from SignalCPU::startup) ---- */
    void resetTo(Addr entryPc);

    /* ---- Outputs ---- */
    Signal<std::optional<FetchWord>> f_word_out;
    Signal<bool>                     f_can_accept;

    /* ---- Input wires (pointed to signals owned by other stages,
     *      bound by Pipeline ctor after all stages exist) ---- */
    void setPopRequestInput(Signal<bool> *s) { _popRequestInput = s; }
    void setERedirectInput(Signal<std::optional<Addr>> *s)
    { _eRedirectInput = s; }
    void setDRedirectInput(Signal<std::optional<Addr>> *s)
    { _dRedirectInput = s; }

    /* ---- Apply a taken-branch redirect.
     *
     *   squashStream(): combinational squash of F's wrong-path
     *     stream state — clears FIFO, drops in-flight tracking,
     *     cancels pending-issue, bumps streamId.  Called from
     *     settle() when a redirect signal arrives; mirrors the
     *     M4's combinational squash on the AHB-issue side.
     *
     *   applyPcUpdate(): latched PC-mux update.  Sets _fetchPc
     *     and _wordFifoMinAddr.  Called from settle() at the cycle
     *     after the redirect signal (the PC mux output is a
     *     pipeline register).
     *
     *   applyRedirect(): convenience wrapper for callers (IRQ
     *     entry, reset) that want the squash and PC update applied
     *     in a single step. ---- */
    void squashStream();
    void applyPcUpdate(Addr target);
    void applyRedirect(Addr target);

    /* ---- Async ingress (called by Pipeline::onAsyncResponse) ---- */
    void recvWordNow(const FetchWord &w);
    void stageForNextCycle(const FetchWord &w);

    /* ---- Bookkeeping called by IcachePort::recvTimingResp ---- */
    void noteResponseArrived();

    /** Called by E when committing a control instruction (branch /
     *  bx / call / etc.).  Decrements F's count of unresolved
     *  branches in flight.  Models the Cortex-M4 PFU's behaviour of
     *  not speculating past an unresolved conditional branch (TRM
     *  §1.4): once predecode at FIFO insertion detects a branch
     *  halfword, F stops issuing further fetches until E confirms
     *  the branch has resolved.  In the taken-branch case
     *  squashStream() also clears the count. */
    void noteBranchCommitted();

    /* ---- Schedule phase: drains pendingIssueQueue ---- */
    bool tryIssuePendingFetch();   // returns false if port refused
    bool hasPendingIssue() const { return !pendingIssueQueue.empty(); }
    PacketPtr peekPendingIssue() const { return pendingIssueQueue.front(); }
    void popPendingIssue() { pendingIssueQueue.pop_front(); }

    /* ---- Read-only query helpers ---- */
    Addr fetchPc() const { return _fetchPc; }
    unsigned fifoSize() const { return fifo.size(); }
    unsigned inFlight() const { return _inFlight; }
    Addr wordFifoMinAddr() const { return _wordFifoMinAddr; }

    /** True if any async-response word is staged for this or next
     *  cycle's settle.  Used by Pipeline's clock-gating predicate to
     *  keep ticking until staged data has been absorbed. */
    bool hasStagedWords() const
    {
        return !currentCycleStaging.empty()
            || !nextCycleStaging.empty();
    }

    /** True if a redirect signal was sampled this cycle but its PC
     *  update hasn't been latched in yet, OR a sampled redirect is
     *  waiting for next-cycle's beginCycle() advance.  Used by
     *  Pipeline's clock-gating predicate to keep the CPU ticking
     *  through the redirect's 1-cycle pipeline register. */
    bool hasPendingRedirect() const
    {
        return _eRedirectPendingThisCycle.has_value()
            || _eRedirectPendingNextCycle.has_value()
            || _dRedirectPendingThisCycle.has_value()
            || _dRedirectPendingNextCycle.has_value();
    }

    /** Snapshot of FIFO contents for line-trace debug.  Returns
     *  {addr, streamId} pairs in head-to-tail order. */
    std::vector<std::pair<Addr, uint32_t>>
    fifoSnapshot() const
    {
        std::vector<std::pair<Addr, uint32_t>> out;
        out.reserve(fifo.size());
        for (const auto &w : fifo)
            out.emplace_back(w.addr, w.streamId);
        return out;
    }

    /** One-line printable state for the line-trace tables.
     *  `detailed` (true under Signal3CPULineTraceDetail) is unused
     *  by Fetch — F has no per-inst metadata — but is part of the
     *  shared snapshotString signature across stages. */
    std::string snapshotString(bool detailed = false) const;

  private:
    SignalCPU &_cpu;
    const unsigned _fifoCapacity;
    const unsigned _maxOutstandingFetches;

    /* Architectural F state (registered conceptually; we maintain
     * the invariant that these are mutated only inside settle() or
     * at reset time, so they behave like pipeline registers). */
    std::deque<FetchWord> fifo;
    Addr     _fetchPc = 0;
    Addr     _wordFifoMinAddr = 0;
    unsigned _inFlight = 0;

    /* Async-ingress staging.  See DESIGN.md §C.4: responses landing
     * within the current cycle window go into currentCycleStaging
     * (drained at the next Settle pass).  Responses that miss the
     * window are deferred to nextCycleStaging, picked up at the top
     * of the next evaluate(). */
    std::vector<FetchWord> currentCycleStaging;
    std::vector<FetchWord> nextCycleStaging;

    /* Outgoing packet queue, flushed by Pipeline during Schedule. */
    std::deque<PacketPtr> pendingIssueQueue;

    /* Re-fire guards (reset at the top of each settle() group). */
    bool _drainedThisSettle = false;

    /* 1-cycle pipeline register on the redirect-to-F path.  Models
     * the register stage between the producer (E's branch resolver
     * or D's tryEarlyResolveBranch) and Fetch's PC-mux input.  A
     * redirect observed by F via _eRedirectInput / _dRedirectInput
     * during settle() at cycle N is *sampled* into the
     * "*PendingNextCycle" slots.  At cycle N+1's beginCycle the
     * sampled value moves to the "*PendingThisCycle" slot, and
     * settle() applies it (clearing the slot once applied).  The
     * `_redirectAppliedThisCycle` flag protects against multiple
     * applies within a single cycle when settle() re-fires through
     * subscribe-and-refire. */
    std::optional<Addr> _eRedirectPendingThisCycle;
    std::optional<Addr> _eRedirectPendingNextCycle;
    std::optional<Addr> _dRedirectPendingThisCycle;
    std::optional<Addr> _dRedirectPendingNextCycle;
    bool                _redirectAppliedThisCycle = false;
    /* Guards squashStream() against being re-fired multiple times
     * within a single settle pass (subscribe-and-refire would
     * otherwise bump streamId on every re-fire).  Reset by
     * beginCycle(). */
    bool                _streamSquashedThisCycle = false;

    /* Number of unresolved branches that F has predecoded out of
     * fetched FIFO words.  F stops issuing new fetches while >0;
     * decremented when E commits a control inst (noteBranchCommitted)
     * and reset to 0 when squashStream() runs on a redirect. */
    unsigned            _branchesInFlight = 0;

    /* Sticky state for predecode: when a 32-bit Thumb-2 inst's
     * first halfword is the upper half of a fetched word, its
     * second half is the lower half of the NEXT fetched word.
     * The next word's lo isn't a 16-bit inst — it's data.  This
     * flag tells the next absorbStaging predecode pass to skip
     * the lo position to avoid false-positive branch flags
     * (e.g. ubfx.w's second half can match a Bcond bit pattern).
     * Reset on squashStream since the redirect target starts a
     * fresh decode flow. */
    bool                _nextWordLoIsThumb2Tail = false;

    /* Per-cycle trace state.  Cleared at the top of each cycle in
     * beginCycle(); populated during settle() / scheduleOutgoing() /
     * absorbStaging().  Used only by snapshotString() / lineTraceEvent
     * to make the per-cycle line trace describe "what happened this
     * cycle" rather than just "end-of-cycle state". */
    std::optional<Addr>      _issuedAddrThisCycle;
    std::vector<Addr>        _arrivedAddrsThisCycle;

    /* Bound by Pipeline ctor to D's d_pop_request output. */
    Signal<bool> *_popRequestInput = nullptr;

    /* Bound by Pipeline ctor to E's e_redirect_to_f output. */
    Signal<std::optional<Addr>> *_eRedirectInput = nullptr;

    /* Bound by Pipeline ctor to D's d_redirect_to_f output. */
    Signal<std::optional<Addr>> *_dRedirectInput = nullptr;

    void absorbStaging();
    bool shouldIssueFetch() const;
    PacketPtr buildFetchPacket();
    void publishHead();
};

} // namespace signal3
} // namespace gem5

#endif // __CPU_SIGNAL_3_STAGE_IN_ORDER_CPU_FETCH_HH__
