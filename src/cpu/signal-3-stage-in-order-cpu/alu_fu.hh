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

namespace gem5
{
namespace signal3
{

class SignalCPU;

/**
 * Basic ALU function unit.
 *
 * Encapsulates integer/branch ALU op execution.  Today every op is
 * 1-cycle (matches Cortex-M4 TRM Table 3-1 for `nop`, `mov`, `add`,
 * `subs`, `cmp`, etc.) so issue and complete happen in the same
 * cycle and the FU is structurally a no-op pass-through.  The shape
 * is the scaffolding for multi-cycle ALU ops (UDIV/SDIV = 2-12 cy
 * per TRM Table 3-1) without changing the existing 1-cy fast path.
 *
 * State machine (mirrors LSQ's three-state pattern):
 *   Idle      — no op in flight; ready to accept a new issue.
 *   Pending   — op issued, latency cycles still to elapse.
 *   Complete  — op finished this cycle; E should commit and release.
 *
 * For 1-cycle ops the FU goes Idle -> Complete inside `issue()` so
 * E sees `complete()` return true immediately (no cycle delay).
 *
 * Supported instruction class (`accepts()`):
 *   - StaticInst::isInteger()  AND
 *   - !StaticInst::isMemRef()  AND
 *   - !StaticInst::isControl()
 * Branches and memrefs are explicitly rejected — Branch resolution
 * lives in E (per TRM Table 3-1's separate "1+P" branch path) and
 * memrefs go through the LSQ.
 *
 * Latency override:
 *   `setLatency(unsigned cycles)` overrides the default 1-cycle path
 *   for testing.  Future calibration may inspect the inst's opcode
 *   to pick a per-op latency (mul=1, udiv=2..12, etc.) — the API is
 *   intentionally minimal for now.
 */
class AluFunctionUnit
{
  public:
    enum class State { Idle, Pending, Complete };

    explicit AluFunctionUnit(SignalCPU &cpu);

    /** True iff this FU should handle `inst`.  Called by E to decide
     *  between the FU path and the legacy direct-execute path. */
    static bool accepts(const StaticInstPtr &inst);

    /** Hand `inst` to the FU.  May complete immediately (1-cy ops)
     *  or move to Pending (multi-cy).  Caller must ensure
     *  `idle()` was true before calling. */
    void issue(const StaticInstPtr &inst);

    /** Per-cycle tick — advances Pending toward Complete.  Called
     *  exactly once per CPU cycle from Pipeline::evaluate before
     *  Settle so `complete()` reflects the current cycle's state. */
    void tick();

    /** Release the result and return the FU to Idle.  Called by E
     *  after it commits the result.  No-op if already Idle. */
    void release();

    /** Reset to Idle (called by Execute::resetTo / IRQ entry). */
    void reset();

    bool idle() const { return _state == State::Idle; }
    bool pending() const { return _state == State::Pending; }
    bool complete() const { return _state == State::Complete; }

    /** Default per-op latency in CPU cycles (1 = 1-cy ALU, the
     *  Cortex-M4 default).  May be overridden for testing. */
    void setLatency(unsigned cycles) { _latency = cycles; }
    unsigned latency() const { return _latency; }

    /** One-line printable state for the line-trace tables. */
    std::string snapshotString() const;

  private:
    SignalCPU &_cpu;

    State _state = State::Idle;
    unsigned _latency = 1;
    unsigned _cyclesLeft = 0;
    StaticInstPtr _inFlight;   // null when Idle/Complete
};

} // namespace signal3
} // namespace gem5

#endif // __CPU_SIGNAL_3_STAGE_IN_ORDER_CPU_ALU_FU_HH__
