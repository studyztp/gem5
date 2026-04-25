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

#ifndef __CPU_SIMPLE_3_CYCLE_IN_ORDER_CPU_PFU_HH__
#define __CPU_SIMPLE_3_CYCLE_IN_ORDER_CPU_PFU_HH__

#include <deque>
#include <optional>
#include <vector>

#include "base/types.hh"
#include "cpu/simple-3-cycle-in-order-cpu/types.hh"
#include "mem/packet.hh"

namespace gem5
{
namespace simple3
{

class Simple3CycleCPU;

/**
 * Prefetch Unit + word FIFO (the F stage).
 *
 * Holds 32-bit fetch words received from the icache.  Decode pulls
 * one word per cycle and feeds its bytes to the generic decoder.
 *
 * The two-level filter (`wordFifoMinAddr` here, `nextInstrAddrToDeliver`
 * in Decode) implements the Python R6 rule that keeps speculative
 * post-branch fetch results that happen to coincide with the
 * mispredicted target's path.
 */
class PFU
{
  public:
    explicit PFU(Simple3CycleCPU &cpu_, unsigned fifoWords);

    /** Reset state for a fresh start (called from CPU startup). */
    void reset(Addr entryPc);

    /** True iff the FIFO can accept at least one more word. */
    bool fifoHasRoom() const;

    /** True iff we should issue a new fetch this cycle: outstanding
     *  in-flight + queued FIFO entries have room for one more, we're
     *  not already past the haltAddr ceiling, and no post-redirect
     *  fetch-restart penalty is still active. */
    bool shouldIssueFetch() const;

    /** Park F-issue for `n` cycles to model the post-redirect AHB /
     *  forwarding latency between a taken-branch resolution and the
     *  first HCLK at which the new ICode fetch can launch. */
    void setRedirectFetchDelay(unsigned n) { _redirectFetchDelay = n; }

    /** Pipeline calls this once per cycle (top of evaluate) to
     *  decrement the redirect-fetch hold-off counter. */
    void tickRedirectDelay()
    {
        if (_redirectFetchDelay > 0)
            --_redirectFetchDelay;
    }

    /** Build a new fetch packet for `_fetchPc` with the given stream
     *  tag and requestor id.  Advances `_fetchPc` by sizeof(uint32_t).
     *  The caller is responsible for sendTimingReq() and for tracking
     *  retry; on a successful send call `noteIssued()`. */
    PacketPtr buildFetchPacket(uint32_t streamId, RequestorID id);

    /** Bookkeeping for in-flight tracking — call when sendTimingReq
     *  returns true. */
    void noteIssued() { ++_numInFlight; }

    /** Bookkeeping when a response arrives (regardless of streamId). */
    void noteResponseArrived() { if (_numInFlight) --_numInFlight; }

    unsigned numInFlight() const { return _numInFlight; }

    /** R6 rule: keep FIFO words whose word_addr >= (target & ~0x3),
     *  reset wordFifoMinAddr/fetchPc to the actual target. */
    void filterFifoAndRedirect(Addr actualPc);

    /** Taken-branch redirect from the D stage's E→D-forwarding
     *  resolution.  Two flavours:
     *
     *    sameLine = false (diff-line target — common case):
     *      Clear the FIFO entirely (the speculative fall-through
     *      stream is discarded), reset fetch state to the new line,
     *      and arm UNCACHEABLE on the next fetch so
     *      PipelinedSimpleMemory pays the full Flash WS latency
     *      (silicon's TAKEN_BRANCH bundle).
     *
     *    sameLine = true (target is in the same 8-byte flash line as
     *    the fall-through):
     *      The line is already in the current-buffer / partially in
     *      our FIFO from speculative fetching past the branch.  Use
     *      `filterFifoAndRedirect` semantics — keep entries with
     *      `addr >= target` — and DO NOT arm UNCACHEABLE.  This
     *      models Python verify_model's MISPREDICT_SAME_LINE_HCLK
     *      = 2 cy cheap bubble. */
    void staticPredictRedirect(Addr targetPc, bool sameLine = false);

    /** Pop the front word for Decode to consume. */
    std::optional<FetchWord> popFrontOfFifo();

    /** Push a word delivered from the icache (or test-injected
     *  in Phase B).  Filters by wordFifoMinAddr. */
    void deliverWord(const FetchWord &word);

    /** Drain the async response-staging vector into the FIFO.  Called
     *  by Pipeline::evaluate() at the top of each cycle. */
    void drainResponseStaging();

    /** Where the next sequential fetch will go. */
    Addr fetchPc() const { return _fetchPc; }

    /** Addresses below this have already been delivered (or skipped
     *  past on a redirect) and should be filtered out at deliver. */
    Addr wordFifoMinAddr() const { return _wordFifoMinAddr; }

    /** Async-arrival staging for icache responses; drained by
     *  Pipeline::evaluate() at step 0. */
    std::vector<FetchWord> responseStaging;

  private:
    Simple3CycleCPU &cpu;
    const unsigned fifoCapacityWords;

    std::deque<FetchWord> fifo;
    Addr _fetchPc;
    Addr _wordFifoMinAddr;

    /** Number of fetches sent on the icache port that haven't received
     *  a response yet.  Includes wrong-path (stream-mismatched)
     *  requests still in flight. */
    unsigned _numInFlight = 0;

    /** Cycles remaining in a post-redirect fetch-restart hold-off.
     *  While > 0, shouldIssueFetch() returns false. */
    unsigned _redirectFetchDelay = 0;

    /** True if the very next fetch we issue should bypass the Flash
     *  read-buffer (set on a taken redirect; cleared after the next
     *  buildFetchPacket). */
    bool _nextFetchBypassBuffer = false;
};

} // namespace simple3
} // namespace gem5

#endif // __CPU_SIMPLE_3_CYCLE_IN_ORDER_CPU_PFU_HH__
