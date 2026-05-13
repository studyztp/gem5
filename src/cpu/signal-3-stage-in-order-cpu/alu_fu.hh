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

#ifndef __CPU_SIGNAL_3_STAGE_IN_ORDER_CPU_ALU_FU_HH__
#define __CPU_SIGNAL_3_STAGE_IN_ORDER_CPU_ALU_FU_HH__

#include <optional>
#include <string>

#include "cpu/static_inst.hh"
#include "sim/eventq.hh"

namespace gem5
{

class ThreadContext;

namespace signal3
{

class SignalCPU;

/**
 * Basic ALU function unit.
 *
 * Encapsulates integer-ALU op execution with per-op latency.
 *
 * Most ARM Cortex-M4 integer ops are 1-cy (nop, mov, add, sub, cmp,
 * mul, ...) and take the same-cycle fast path: `issue()` transitions
 * Idle -> Complete in one call so E commits in the same cycle as
 * issue.  SDIV/UDIV are 2-12 cy keyed on the dividend's significant
 * bits (DDI0439D Table 3-1); those go Idle -> Pending and schedule
 * a single completion event at `clockEdge(Cycles(lat - 1))`.  E
 * stalls (writes e_accept_d=false) while Pending, then commits on
 * the cycle the event fires.
 *
 * State machine:
 *   Idle      — no op in flight; ready to accept a new issue.
 *   Pending   — op issued, completion event scheduled, cycles to go.
 *   Complete  — op finished this cycle; E should commit and release.
 *
 * Supported instruction class (`accepts()`):
 *   - (StaticInst::isInteger() OR StaticInst::isFloating()) AND
 *   - !StaticInst::isMemRef()  AND
 *   - !StaticInst::isControl()
 * Branches go through E's direct path, memrefs through the LSQ.
 * Floating-point ALU ops are routed through this FU; their per-op
 * latency comes from the SignalCPU's per-OpClass table (see
 * signal_cpu.hh `opClassLatency()`).
 */
class AluFunctionUnit
{
  public:
    enum class State { Idle, Pending, Complete };

    explicit AluFunctionUnit(SignalCPU &cpu);

    /** True iff this FU should handle `inst`.  Called by E to decide
     *  between the FU path and the legacy direct-execute path. */
    static bool accepts(const StaticInstPtr &inst);

    /** Hand `inst` to the FU.  Reads `inst`'s source register values
     *  via `tc` to compute per-op latency.  May complete immediately
     *  (1-cy ops) or move to Pending (multi-cy: SDIV/UDIV).  Caller
     *  must ensure `idle()` was true before calling. */
    void issue(const StaticInstPtr &inst, ThreadContext *tc);

    /** Release the result and return the FU to Idle.  Called by E
     *  after it commits the result.  No-op if already Idle. */
    void release();

    /** Reset to Idle (called by Execute::resetTo / IRQ entry).
     *  Deschedules any pending completion event. */
    void reset();

    bool idle() const { return _state == State::Idle; }
    bool pending() const { return _state == State::Pending; }
    bool complete() const { return _state == State::Complete; }

    /** One-line printable state for the line-trace tables. */
    std::string snapshotString() const;

    /** Compact form for the action-led execute line-trace column.
     *  Returns "I" / "P(k/L)" / "C" where k is cycles-left and L is
     *  the scheduled latency. */
    std::string compactStateString() const;

  private:
    SignalCPU &_cpu;

    State _state = State::Idle;
    StaticInstPtr _inFlight;        // null when Idle/Complete

    /** Single-shot completion event scheduled at issue time of a
     *  multi-cy op.  Same instance is reused across ops because
     *  `issue()` only fires when `_state == Idle` (panic otherwise),
     *  which guarantees the event is not already scheduled. */
    EventFunctionWrapper _completeEvent;

    /** Cycle count selected at the most recent multi-cy issue.
     *  Pure logging aid for `snapshotString()` and trace output —
     *  the cycle-by-cycle state is owned by the gem5 event queue. */
    unsigned _scheduledLatency = 0;

    /** Compute the per-inst cycle latency.  Returns 1 for every
     *  accepted op except `IntDivOp`, which gets the Cortex-M4 TRM
     *  Table 3-1 formula `clamp(4 + ceil(sigBits/4), 2, 12)` keyed on
     *  the dividend's significant bits. */
    unsigned latencyFor(const StaticInstPtr &inst,
                        ThreadContext *tc) const;

    /** Body of `_completeEvent`: Pending -> Complete + wake the
     *  pipeline so E commits this cycle. */
    void onComplete();
};

} // namespace signal3
} // namespace gem5

#endif // __CPU_SIGNAL_3_STAGE_IN_ORDER_CPU_ALU_FU_HH__
