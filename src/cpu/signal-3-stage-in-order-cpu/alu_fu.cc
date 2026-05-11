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

#include "cpu/signal-3-stage-in-order-cpu/alu_fu.hh"

#include <algorithm>
#include <cstdlib>
#include <sstream>

#include "base/logging.hh"
#include "cpu/op_class.hh"
#include "cpu/reg_class.hh"
#include "cpu/signal-3-stage-in-order-cpu/pipeline.hh"
#include "cpu/signal-3-stage-in-order-cpu/signal_cpu.hh"
#include "cpu/thread_context.hh"
#include "debug/Signal3CPUAluFU.hh"

namespace gem5
{
namespace signal3
{

AluFunctionUnit::AluFunctionUnit(SignalCPU &cpu)
    : _cpu(cpu),
      // Default_Pri (0) fires before CPU_Tick_Pri (50) at the same
      // Tick, so when the event fires at clockEdge(Cycles(lat-1)) the
      // state is Complete *before* the pipeline's tick observes the
      // FU this cycle.  See plan behavior-matrix case 7.
      _completeEvent([this]{ onComplete(); },
                     cpu.name() + ".aluFu.complete",
                     /*del=*/false, Event::Default_Pri)
{
}

bool
AluFunctionUnit::accepts(const StaticInstPtr &inst)
{
    if (!inst)
        return false;
    // Memory ops go through the LSQ; control ops (branches) are
    // resolved directly in E because the redirect path is structural
    // (e_redirect_to_f / d_redirect_to_f).  Floating-point ops aren't
    // ALU and aren't modeled here yet.
    if (inst->isMemRef())
        return false;
    if (inst->isControl())
        return false;
    if (inst->isFloating())
        return false;
    return inst->isInteger();
}

unsigned
AluFunctionUnit::latencyFor(const StaticInstPtr &inst,
                            ThreadContext *tc) const
{
    // Every accepted op except IntDivOp is 1-cy on Cortex-M4 — this
    // FU stays a same-cycle pass-through for the common case.
    if (inst->opClass() != IntDivOp)
        return 1;

    // Find the maximum-magnitude integer source operand.  ARM SDIV /
    // UDIV take two int sources (dividend Rn, divisor Rm); we read
    // both and use the larger absolute value, conservatively matching
    // MinorCPU's DynamicLatencyIntDivFU.  See behavior-matrix case 2.
    uint32_t maxVal = 0;
    for (int i = 0; i < inst->numSrcRegs(); i++) {
        RegId r = inst->srcRegIdx(i);
        if (r.classValue() != IntRegClass)
            continue;
        uint32_t v = (uint32_t)tc->getReg(r);
        uint32_t a;
        // std::abs(INT_MIN) is undefined; |INT_MIN| == 0x80000000 fits
        // in a uint32_t but not an int32_t.  Behavior-matrix case 4.
        if (v == 0x80000000u)
            a = 0x80000000u;
        else
            a = (uint32_t)std::abs((int32_t)v);
        if (a > maxVal)
            maxVal = a;
    }

    unsigned sigBits = (maxVal == 0) ? 0 : (32 - __builtin_clz(maxVal));
    // Case 3: dividend == 0 yields the minimum-latency 2-cy path.
    // Otherwise the M4 TRM formula 4 + ceil(sigBits/4) clamped to
    // [2, 12].  Coefficients reproduce silicon on bench-div (sigBits
    // 31 -> 12 cy) and bench-div_short (sigBits 4 -> 5 cy).
    unsigned lat = (maxVal == 0) ? 2
        : std::clamp(4u + (sigBits + 3) / 4, 2u, 12u);

    DPRINTF(Signal3CPUAluFU,
            "latencyFor pc=%#x op=%s maxVal=%#x sigBits=%u lat=%u\n",
            (unsigned)tc->pcState().instAddr(),
            inst->getName().c_str(), maxVal, sigBits, lat);
    return lat;
}

void
AluFunctionUnit::issue(const StaticInstPtr &inst, ThreadContext *tc)
{
    panic_if(_state != State::Idle,
             "AluFunctionUnit::issue called while not Idle (state=%d)",
             (int)_state);
    panic_if(!accepts(inst),
             "AluFunctionUnit::issue: inst doesn't pass accepts()");
    // Behavior-matrix case 20: belt-and-suspenders guard so a future
    // refactor that calls issue() outside Idle is caught early.
    panic_if(_completeEvent.scheduled(),
             "AluFunctionUnit::issue: completion event already scheduled");

    _inFlight = inst;
    unsigned lat = latencyFor(inst, tc);
    _scheduledLatency = lat;

    if (lat <= 1) {
        // 1-cy fast path: complete same cycle as issue.  E sees
        // complete() return true immediately and commits without
        // any cycle delay (preserves existing nop/mov/add timing).
        _state = State::Complete;
        DPRINTF(Signal3CPUAluFU,
                "issue pc=%#x op=%s lat=1 -> Complete (same-cycle)\n",
                (unsigned)tc->pcState().instAddr(),
                inst->getName().c_str());
        return;
    }

    _state = State::Pending;
    // Fire at the cycle edge (lat - 1) cycles from now.  The event
    // fires at the start of that cycle (Default_Pri = 0 < CPU_Tick_Pri
    // = 50) so E's settle in that same cycle observes Complete and
    // commits.  Total cycles occupied at E: lat.
    Tick when = _cpu.clockEdge(Cycles(lat - 1));
    _cpu.schedule(_completeEvent, when);
    DPRINTF(Signal3CPUAluFU,
            "issue pc=%#x op=%s lat=%u -> Pending; complete@tick=%llu\n",
            (unsigned)tc->pcState().instAddr(),
            inst->getName().c_str(), lat,
            (unsigned long long)when);
}

void
AluFunctionUnit::onComplete()
{
    panic_if(_state != State::Pending,
             "AluFunctionUnit::onComplete: state=%d (expected Pending)",
             (int)_state);
    _state = State::Complete;
    DPRINTF(Signal3CPUAluFU,
            "complete (sched_lat=%u) -> Complete; requesting retick\n",
            _scheduledLatency);
    // Defensive wake-up.  Under current Pipeline::safeToIdle()
    // semantics the CPU isn't idle while _eSlot is occupied, so the
    // settle pass this cycle would commit naturally.  A future
    // refactor that changes busy() could break that invariant; the
    // retick costs ~nothing and matches LSQ's completionSignal
    // wake-up pattern.  See behavior-matrix case 18.
    _cpu.pipeline().requestRetick("alu_complete");
}

void
AluFunctionUnit::release()
{
    if (_state == State::Idle)
        return;
    DPRINTF(Signal3CPUAluFU, "release\n");
    _state = State::Idle;
    _inFlight = nullptr;
    _scheduledLatency = 0;
    // E only calls release() after observing Complete, so the event
    // should already have fired.  Descheduling here would only matter
    // under a future refactor that releases mid-Pending — keep the
    // guard for safety, matching reset().
    if (_completeEvent.scheduled())
        _cpu.deschedule(_completeEvent);
}

void
AluFunctionUnit::reset()
{
    // Behavior-matrix case 15 / 16: IRQ entry, pipeline reset, or
    // takeover discards any in-flight ALU op.  On resume the
    // architectural PC re-fetches the same inst, which re-issues
    // and re-runs the full latency.  Deterministic re-execution
    // matches MinorCPU's semantics.
    if (_completeEvent.scheduled())
        _cpu.deschedule(_completeEvent);
    _state = State::Idle;
    _inFlight = nullptr;
    _scheduledLatency = 0;
}

std::string
AluFunctionUnit::snapshotString() const
{
    std::ostringstream os;
    os << "alu=";
    switch (_state) {
      case State::Idle:     os << "Idle"; break;
      case State::Pending:
        os << "Pending(";
        if (_completeEvent.scheduled()) {
            Tick remaining = _completeEvent.when() - curTick();
            Tick period = _cpu.clockPeriod();
            unsigned cyclesLeft =
                period ? (unsigned)((remaining + period - 1) / period)
                       : 0;
            os << cyclesLeft;
        } else {
            os << "?";
        }
        os << "/" << _scheduledLatency << ")";
        break;
      case State::Complete: os << "Complete"; break;
    }
    return os.str();
}

} // namespace signal3
} // namespace gem5
