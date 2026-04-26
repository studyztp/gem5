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
    Fetch(SignalGraph &g, SignalCPU &cpu, unsigned fifoCapacity);

    void settle() override;

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

    /* ---- Apply a taken-branch redirect (called from settle when
     *      e_redirect_to_f is asserted, and from external test
     *      hooks). ---- */
    void applyRedirect(Addr target);

    /* ---- Async ingress (called by Pipeline::onAsyncResponse) ---- */
    void recvWordNow(const FetchWord &w);
    void stageForNextCycle(const FetchWord &w);

    /* ---- Bookkeeping called by IcachePort::recvTimingResp ---- */
    void noteResponseArrived();

    /* ---- Schedule phase: drains pendingIssueQueue ---- */
    bool tryIssuePendingFetch();   // returns false if port refused
    bool hasPendingIssue() const { return !pendingIssueQueue.empty(); }
    PacketPtr peekPendingIssue() const { return pendingIssueQueue.front(); }
    void popPendingIssue() { pendingIssueQueue.pop_front(); }

    /* ---- Read-only query helpers ---- */
    Addr fetchPc() const { return _fetchPc; }
    unsigned fifoSize() const { return fifo.size(); }
    unsigned inFlight() const { return _inFlight; }

  private:
    SignalCPU &_cpu;
    const unsigned _fifoCapacity;

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

    /* Bound by Pipeline ctor to D's d_pop_request output. */
    Signal<bool> *_popRequestInput = nullptr;

    /* Bound by Pipeline ctor to E's e_redirect_to_f output. */
    Signal<std::optional<Addr>> *_eRedirectInput = nullptr;

    /* Bound by Pipeline ctor to D's d_redirect_to_f output (S5). */
    Signal<std::optional<Addr>> *_dRedirectInput = nullptr;

    /* S6: when set, the next outgoing fetch is marked UNCACHEABLE so
     * PipelinedSimpleMemory bypasses its 64-bit current-line buffer
     * and pays the full Flash WS latency.  Models the silicon
     * TAKEN_BRANCH HCLK bundle on the post-redirect target fetch. */
    bool _nextFetchBypassBuffer = false;

    void absorbStaging();
    bool shouldIssueFetch() const;
    PacketPtr buildFetchPacket();
    void publishHead();
};

} // namespace signal3
} // namespace gem5

#endif // __CPU_SIGNAL_3_STAGE_IN_ORDER_CPU_FETCH_HH__
