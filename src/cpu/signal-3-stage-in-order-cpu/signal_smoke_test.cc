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

#include "cpu/signal-3-stage-in-order-cpu/signal_smoke_test.hh"

#include "base/logging.hh"
#include "debug/Signal3CPU.hh"
#include "debug/Signal3CPULineTrace.hh"
#include "debug/Signal3CPUPipeline.hh"
#include "sim/sim_exit.hh"

namespace gem5
{
namespace signal3
{

/* ===== Counter ===== */

SignalSmokeTest::Counter::Counter(SignalGraph &g, Latch<int> &latchRef)
    : Stage(g, "counter", /*stageId=*/0), _latch(latchRef)
{
}

void
SignalSmokeTest::Counter::settle()
{
    // Drive the next integer into the latch input.  The latch will
    // sample it at the next Edge.
    _latch.in().write(_next);
    DPRINTF(Signal3CPU, "counter wrote next=%d to latch.in\n", _next);
    ++_next;
}

/* ===== Printer ===== */

SignalSmokeTest::Printer::Printer(SignalGraph &g, Latch<int> &latchRef,
                                  int *observedSink, int *errorCountSink)
    : Stage(g, "printer", /*stageId=*/1),
      _latch(latchRef),
      _observedSink(observedSink),
      _errorCountSink(errorCountSink)
{
    // Subscribe to the latch output so we re-fire whenever the
    // sampled value changes.
    _latch.out().subscribe(this);
}

void
SignalSmokeTest::Printer::settle()
{
    if (!_latch.out().valid()) {
        DPRINTF(Signal3CPU, "printer: latch.out not valid yet\n");
        return;
    }
    int v = _latch.out().read();
    if (v == _lastSeen) {
        // No new value this cycle (latch held).
        return;
    }
    _lastSeen = v;
    *_observedSink = v;
    if (v != _expected) {
        ++(*_errorCountSink);
        DPRINTF(Signal3CPU,
                "printer ERROR: expected=%d observed=%d\n", _expected, v);
    } else {
        DPRINTF(Signal3CPU, "printer ok value=%d\n", v);
    }
    ++_expected;
}

/* ===== SignalSmokeTest SimObject ===== */

SignalSmokeTest::SignalSmokeTest(const Params &p)
    : ClockedObject(p),
      _graph(),
      _latch(_graph, "counterLatch"),
      _tickEvent([this]() { tick(); }, name() + ".tick"),
      _asyncTriggerEvent([this]() { asyncTrigger(); },
                         name() + ".asyncTrigger"),
      _numCycles(p.num_cycles),
      _asyncTriggerOffsetTicks(p.async_trigger_offset_ticks),
      _asyncTriggerCycle(p.async_trigger_cycle),
      _runAsyncTest(p.run_async_test)
{
    _counter = std::make_unique<Counter>(_graph, _latch);
    _printer = std::make_unique<Printer>(_graph, _latch,
                                         &_lastObserved, &_errorCount);
}

void
SignalSmokeTest::startup()
{
    // Schedule the first tick at the next clock edge.
    schedule(_tickEvent, clockEdge(Cycles(1)));
    if (_runAsyncTest) {
        // Schedule a one-shot async trigger inside the cycle that
        // begins at clockEdge(Cycles(asyncTriggerCycle)).  We add
        // an offset (in ticks, NOT cycles) within that cycle.
        Tick fireAt = clockEdge(_asyncTriggerCycle)
                      + _asyncTriggerOffsetTicks;
        schedule(_asyncTriggerEvent, fireAt);
        DPRINTF(Signal3CPU,
                "scheduled async trigger at tick=%llu "
                "(cycle=%llu + offset=%llu)\n",
                (unsigned long long)fireAt,
                (unsigned long long)(uint64_t)_asyncTriggerCycle,
                (unsigned long long)_asyncTriggerOffsetTicks);
    }
}

void
SignalSmokeTest::tick()
{
    _graph.beginCycle(curTick(), clockPeriod());
    _graph.edge();
    _graph.settle();
    DPRINTF(Signal3CPULineTrace,
            "cyc=%d observed=%d expected=%d errors=%d\n",
            _cyclesRun, _lastObserved, _cyclesRun - 1, _errorCount);

    ++_cyclesRun;
    if (_cyclesRun >= (int)_numCycles) {
        // Final assertion: printer must have observed _numCycles - 1
        // values (one per latch-Edge, except cycle 0 where the latch
        // hadn't sampled anything yet).  Errors must be zero.  And
        // the async-test (if run) must have fired in-window.
        const int expectedLast = _cyclesRun - 2; // last printed value
        bool ok = (_errorCount == 0) && (_lastObserved == expectedLast);
        if (_runAsyncTest)
            ok = ok && (_asyncFiredInWindow == 1);
        if (!ok) {
            warn("SignalSmokeTest FAIL: cyclesRun=%d lastObserved=%d "
                 "expectedLast=%d errors=%d asyncFiredInWindow=%d\n",
                 _cyclesRun, _lastObserved, expectedLast,
                 _errorCount, _asyncFiredInWindow);
        }
        exitSimLoop(ok ? "smoke-test-pass" : "smoke-test-fail");
        return;
    }
    schedule(_tickEvent, clockEdge(Cycles(1)));
}

void
SignalSmokeTest::asyncTrigger()
{
    // Verify isWithinCycle classifies us correctly.  curTick() must
    // satisfy [cycleStart, cycleStart + period).
    const Tick t = curTick();
    const bool inWindow = _graph.isWithinCycle(t);
    _asyncFiredInWindow = inWindow ? 1 : -1;
    DPRINTF(Signal3CPU,
            "asyncTrigger fired tick=%llu cycleStart=%llu period=%llu "
            "inWindow=%d\n",
            (unsigned long long)t,
            (unsigned long long)_graph.cycleStartTick(),
            (unsigned long long)_graph.cyclePeriodTicks(),
            (int)inWindow);
    if (!inWindow) {
        warn("asyncTrigger out of window (S0 setup error?)\n");
    }
}

} // namespace signal3
} // namespace gem5
