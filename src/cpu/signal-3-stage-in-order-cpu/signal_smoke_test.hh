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

#ifndef __CPU_SIGNAL_3_STAGE_IN_ORDER_CPU_SIGNAL_SMOKE_TEST_HH__
#define __CPU_SIGNAL_3_STAGE_IN_ORDER_CPU_SIGNAL_SMOKE_TEST_HH__

#include <memory>

#include "cpu/signal-3-stage-in-order-cpu/signal.hh"
#include "params/SignalSmokeTest.hh"
#include "sim/clocked_object.hh"
#include "sim/eventq.hh"
#include "sim/ticked_object.hh"

namespace gem5
{
namespace signal3
{

/**
 * Smoke test for the Signal/Latch primitive.
 *
 * Pipeline:
 *   counter --(int)--> Latch<int> --(out)--> printer
 *
 * Counter increments its input every cycle.  The Latch samples on
 * Edge so the Printer sees N at cycle N+1 (where the Latch was
 * sampled at the Edge of cycle N+1 from Counter's write at cycle N).
 *
 * To exercise the mid-cycle async-response path the test object can
 * be configured to schedule a one-shot "async event" at a tick
 * partway through a target cycle.  When that event fires it calls
 * SignalGraph::isWithinCycle (must return true), invokes settle()
 * via an "external trigger" path, and verifies that an extra Settle
 * pass executed without panicking.
 */
class SignalSmokeTest : public ClockedObject
{
  public:
    PARAMS(SignalSmokeTest);
    SignalSmokeTest(const Params &p);

    void startup() override;

  private:
    /* ---- Stage classes ---- */

    class Counter : public Stage
    {
      public:
        Counter(SignalGraph &g, Latch<int> &latchRef);
        void settle() override;
      private:
        Latch<int> &_latch;
        int _next = 0;
    };

    class Printer : public Stage
    {
      public:
        Printer(SignalGraph &g, Latch<int> &latchRef,
                int *observedSink, int *errorCountSink);
        void settle() override;
      private:
        Latch<int> &_latch;
        int *_observedSink;
        int *_errorCountSink;
        int _expected = 0;
        int _lastSeen = -1;  // -1 = "no value seen yet"
    };

    /* ---- Tick driver: a thin handwritten Ticked wrapper using
     *      ClockedObject's event queue.  We don't subclass Ticked
     *      itself in S0 to keep the test free-standing. ---- */

    void tick();

    SignalGraph _graph;
    Latch<int> _latch;
    std::unique_ptr<Counter> _counter;
    std::unique_ptr<Printer> _printer;

    EventFunctionWrapper _tickEvent;
    EventFunctionWrapper _asyncTriggerEvent;

    void asyncTrigger();

    /* ---- Knobs from Python ---- */

    const Cycles _numCycles;
    const Tick   _asyncTriggerOffsetTicks;
    const Cycles _asyncTriggerCycle;
    const bool   _runAsyncTest;

    /* ---- Observability for the test harness ---- */

    int _cyclesRun = 0;
    int _lastObserved = -1;
    int _errorCount = 0;
    int _asyncFiredInWindow = 0; // 0 = not run, 1 = fired in window, -1 = out
};

} // namespace signal3
} // namespace gem5

#endif // __CPU_SIGNAL_3_STAGE_IN_ORDER_CPU_SIGNAL_SMOKE_TEST_HH__
