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

#ifndef __CPU_SIMPLE_3_CYCLE_IN_ORDER_CPU_DECODE_HH__
#define __CPU_SIMPLE_3_CYCLE_IN_ORDER_CPU_DECODE_HH__

#include <optional>

#include "base/types.hh"
#include "cpu/simple-3-cycle-in-order-cpu/types.hh"

namespace gem5
{

class ThreadContext;

namespace simple3
{

class PFU;
class Simple3CycleCPU;

/**
 * Decode stage: holds one D slot, runs static branch prediction,
 * tracks the post-mispredict flush bubble.
 *
 * Phase-A stub: most methods do nothing.  Real implementation comes
 * online in Phase B (decoder driving) and C (mispredict + redirect).
 */
class Decode
{
  public:
    explicit Decode(Simple3CycleCPU &cpu_);

    void reset();

    /** True iff D has an instruction queued and ready to advance to E. */
    bool hasInstrReadyForExecute() const { return dSlot.has_value(); }

    /** Pop the D slot for promotion to E. */
    DecodedSlotEntry popForExecute();

    /** D-stage step: pull a word from PFU (if available), feed it
     *  to the decoder, and if instReady() set up dSlot. */
    void stepOneInstr(PFU &pfu, ThreadContext *tc);

    /** Mispredict: drop the (possibly speculative) D slot.  Also
     *  invalidates the cached fetch word so a redirect won't re-feed
     *  stale bytes into the decoder. */
    void squashDSlot()
    {
        dSlot.reset();
        resetFetchTracking();
    }

    /** Mark the dSlot inst as early-resolved (Pipeline's E→D forwarding
     *  step ran its execute() at D-time and computed the next PC). */
    void markSlotEarlyResolved(Addr resolvedNpc)
    {
        if (dSlot) {
            dSlot->earlyResolved = true;
            dSlot->resolvedNpc = resolvedNpc;
        }
    }

    /** Begin a flush bubble of N cycles during which FIFO->D is
     *  inhibited. */
    void startFlushBubble(Cycles n) { flushRemaining = n; }

    /** Decrement the flush counter (Pipeline calls once per cycle). */
    void decrementFlushCounter()
    {
        if (flushRemaining > Cycles(0))
            --flushRemaining;
    }

    bool isFlushing() const { return flushRemaining > Cycles(0); }
    bool busy() const { return dSlot.has_value(); }

    Addr nextInstrAddrToDeliver() const { return _nextInstrAddrToDeliver; }
    void setNextInstrAddrToDeliver(Addr a) { _nextInstrAddrToDeliver = a; }
    const std::optional<DecodedSlotEntry> &slot() const { return dSlot; }

    /** Reset decoder fetch tracking after a redirect or squash. */
    void resetFetchTracking()
    {
        _haveLoadedWord = false;
        _lastFetchPc = 0;
    }

  private:
    Simple3CycleCPU &cpu;

    std::optional<DecodedSlotEntry> dSlot;
    Cycles flushRemaining;

    /** Per-instruction filter complementing PFU::wordFifoMinAddr.
     *  Drops instructions in a delivered word whose addr is below
     *  the current target. */
    Addr _nextInstrAddrToDeliver;

    /** Tracks the last fetch word fed to the decoder.  After a
     *  decode() pops an instruction, the same word may still hold
     *  more bytes (e.g. the upper Thumb halfword); we re-feed
     *  via moreBytes(pc, lastFetchPc) so the decoder recomputes
     *  its offset for the next instruction. */
    bool _haveLoadedWord = false;
    Addr _lastFetchPc = 0;
};

} // namespace simple3
} // namespace gem5

#endif // __CPU_SIMPLE_3_CYCLE_IN_ORDER_CPU_DECODE_HH__
