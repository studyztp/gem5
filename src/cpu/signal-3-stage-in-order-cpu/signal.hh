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

#ifndef __CPU_SIGNAL_3_STAGE_IN_ORDER_CPU_SIGNAL_HH__
#define __CPU_SIGNAL_3_STAGE_IN_ORDER_CPU_SIGNAL_HH__

#include <algorithm>
#include <functional>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "base/logging.hh"
#include "base/trace.hh"
#include "base/types.hh"
#include "debug/Signal3CPULatch.hh"
#include "debug/Signal3CPUPipeline.hh"
#include "debug/Signal3CPUSignal.hh"

namespace gem5
{
namespace signal3
{

class Stage;
class SignalGraph;

/**
 * Type-erased base for all Signal<T> instances so SignalGraph can
 * track every signal it owns (for per-cycle pulse reset).
 */
class ISignalBase
{
  public:
    virtual ~ISignalBase() = default;
    virtual const std::string &name() const = 0;
    virtual bool isPulse() const = 0;
    virtual void resetPulse() = 0;
};

/**
 * Base for any pipeline stage in the signal-driven CPU.  Stages
 * register themselves with the SignalGraph and implement a single
 * `settle()` method that is re-fired whenever any of their input
 * signals change during the Settle phase.
 *
 * stageId orders Settle iteration deterministically (smaller first).
 * Convention: F=0, D=1, E=2.
 */
class Stage
{
  public:
    Stage(SignalGraph &g, const std::string &nm, int id);
    virtual ~Stage() = default;

    virtual void settle() = 0;

    const std::string &name() const { return _name; }
    int stageId() const { return _stageId; }
    SignalGraph &graph() { return _graph; }

  protected:
    SignalGraph &_graph;
    const std::string _name;
    const int _stageId;
};

/**
 * A combinational wire carrying a value of type T.
 *
 * write(v) is anti-cascade guarded by operator==: writing the same
 * value twice in the same Settle pass is a no-op, which prevents
 * infinite cascades when a downstream stage ends up writing back to
 * an upstream one with no actual change.
 *
 * Subscribers are Stages that read this Signal; they are queued
 * (via SignalGraph::markDirty) for re-fire when the value changes.
 *
 * `pulse` signals (e.g. d_redirect_to_f) are reset to invalid at
 * SignalGraph::beginCycle so a stale Settle-phase assertion does
 * not bleed into the next cycle.
 *
 * `fromStage` is the producing stage and is used to detect (at
 * subscribe-time) accidental self-loops where a stage subscribes to
 * its own output.  Pass nullptr for signals that have no single
 * producing stage (e.g. Latch outputs).
 */
template <typename T>
class Signal : public ISignalBase
{
  public:
    Signal(SignalGraph &g, const std::string &nm,
           Stage *fromStage = nullptr, bool pulse = false);
    ~Signal() override = default;

    /** Equality-guarded write.  Returns true if the value actually
     *  changed (and the cascade was triggered). */
    bool write(const T &v);

    const T &read() const { return _value; }
    bool valid() const { return _valid; }

    void subscribe(Stage *consumer);

    const std::string &name() const override { return _name; }
    bool isPulse() const override { return _pulse; }
    void resetPulse() override { if (_pulse) _valid = false; }

  private:
    SignalGraph &_graph;
    const std::string _name;
    Stage *const _fromStage;
    const bool _pulse;
    T _value{};
    bool _valid = false;
    std::vector<Stage *> _subscribers;
};

/**
 * A pipeline register: input is sampled at the next clock Edge and
 * presented on output until the following Edge.  Stages write to
 * `in()` during Settle; downstream stages read `out()`.
 *
 * If a Latch's input is never written during a cycle, commitEdge()
 * preserves the previous output value (i.e. the register holds).
 */
template <typename T>
class Latch
{
  public:
    Latch(SignalGraph &g, const std::string &nm,
          Stage *fromStage = nullptr);

    Signal<T> &in() { return _input; }
    const Signal<T> &out() const { return _output; }
    Signal<T> &out() { return _output; }

    /** Capture the input (if it was written this cycle) and write
     *  it through to the output Signal, triggering subscribers if
     *  the output value changed. */
    void commitEdge();

    const std::string &name() const { return _name; }

  private:
    const std::string _name;
    Signal<T> _input;
    Signal<T> _output;
};

/**
 * Per-Pipeline registry of Signals, Latches, and Stages.  Owns the
 * cycle-window state used for mid-cycle async-response classification
 * and drives the Edge / Settle phases.
 */
class SignalGraph
{
  public:
    static constexpr int kMaxSettleIters = 4;

    SignalGraph();

    /* ---- Lifecycle (called by Stage / Signal / Latch ctors) ---- */

    void registerStage(Stage *s);
    void registerLatchCommit(std::function<void()> commitEdge,
                             const std::string &nm);
    void registerSignal(ISignalBase *s);

    /* ---- Per-cycle driving API (called by Pipeline) ---- */

    /**
     * Open a new cycle: record the start tick and the period (so
     * isWithinCycle() can answer mid-cycle queries), reset every
     * pulse signal, and seed the dirty-stage set with all stages
     * for the upcoming Edge+Settle pass.
     */
    void beginCycle(Tick startTick, Tick periodTicks);

    /** Sample every Latch's input into its output, firing the
     *  cascade for any value that changed. */
    void edge();

    /** Bounded fixpoint over dirtyStages.  Panics if the cap is hit
     *  (indicates a non-converging combinational loop — a model bug). */
    void settle();

    /* ---- Mid-cycle window query (Rev 2 issue #2) ---- */

    /** Half-open interval: [cycleStart, cycleStart + period). */
    bool isWithinCycle(Tick t) const;
    Tick cycleStartTick() const { return _cycleStart; }
    Tick cyclePeriodTicks() const { return _cyclePeriod; }

    /* ---- Re-entrancy state (mid-cycle responses) ---- */

    bool inSettle() const { return _inSettle; }

    /* ---- Called by Signal<T>::write on a real change ---- */

    void markDirty(Stage *s);

    /** For DPRINTF dumps. */
    std::string dirtyStagesString() const;

  private:
    Tick _cycleStart = 0;
    Tick _cyclePeriod = 0;
    bool _inSettle = false;

    std::vector<Stage *> _stages;
    std::vector<std::function<void()>> _latchCommits;
    std::vector<std::string> _latchNames;
    std::vector<ISignalBase *> _signals;

    /* Sorted by stageId for deterministic iteration. */
    struct StageOrder
    {
        bool operator()(Stage *a, Stage *b) const;
    };
    std::set<Stage *, StageOrder> _dirtyStages;
};


/* ===========================================================
 *                Inline / template definitions
 * ========================================================= */

inline
Stage::Stage(SignalGraph &g, const std::string &nm, int id)
    : _graph(g), _name(nm), _stageId(id)
{
    g.registerStage(this);
}

template <typename T>
Signal<T>::Signal(SignalGraph &g, const std::string &nm,
                  Stage *fromStage, bool pulse)
    : _graph(g), _name(nm), _fromStage(fromStage), _pulse(pulse)
{
    _graph.registerSignal(this);
}

template <typename T>
bool
Signal<T>::write(const T &v)
{
    // Anti-cascade equality guard: a re-fired stage that produces
    // the same output must not re-trigger downstream Settle work.
    if (_valid && _value == v) {
        DPRINTF(Signal3CPUSignal,
                "signal=%s write same value (no cascade)\n",
                _name.c_str());
        return false;
    }
    _value = v;
    _valid = true;
    DPRINTF(Signal3CPUSignal,
            "signal=%s changed; %lu subscribers re-queued\n",
            _name.c_str(), _subscribers.size());
    for (Stage *s : _subscribers)
        _graph.markDirty(s);
    return true;
}

template <typename T>
void
Signal<T>::subscribe(Stage *consumer)
{
    panic_if(consumer == nullptr,
             "Signal '%s'::subscribe(nullptr)", _name.c_str());
    panic_if(_fromStage != nullptr && consumer == _fromStage,
             "Signal '%s' would self-loop: stage '%s' subscribes to "
             "its own output", _name.c_str(),
             consumer->name().c_str());
    _subscribers.push_back(consumer);
}

template <typename T>
Latch<T>::Latch(SignalGraph &g, const std::string &nm,
                Stage *fromStage)
    : _name(nm),
      _input(g, nm + ".in", fromStage, /*pulse=*/false),
      _output(g, nm + ".out", /*fromStage=*/nullptr, /*pulse=*/false)
{
    g.registerLatchCommit([this]() { this->commitEdge(); }, nm);
}

template <typename T>
void
Latch<T>::commitEdge()
{
    if (!_input.valid()) {
        DPRINTF(Signal3CPULatch,
                "latch=%s no input written this cycle (hold)\n",
                _name.c_str());
        return;
    }
    const bool changed = _output.write(_input.read());
    DPRINTF(Signal3CPULatch,
            "latch=%s commit (output %s)\n",
            _name.c_str(), changed ? "changed" : "unchanged");
}

inline bool
SignalGraph::StageOrder::operator()(Stage *a, Stage *b) const
{
    if (a->stageId() != b->stageId())
        return a->stageId() < b->stageId();
    return a < b;
}

} // namespace signal3
} // namespace gem5

#endif // __CPU_SIGNAL_3_STAGE_IN_ORDER_CPU_SIGNAL_HH__
