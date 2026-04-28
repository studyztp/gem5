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

#ifndef __CPU_LEGO_STAGE_HH__
#define __CPU_LEGO_STAGE_HH__

#include <any>
#include <string>
#include <unordered_map>

namespace gem5
{

/**
 * A StageState is a named bag of fields shared among function units
 * inside a single stage during a tick. It is the primary medium for
 * sharing values between FUs that live in the same stage.
 *
 * Fields are typed at the access site: set<T>/get<T> use std::any,
 * giving runtime flexibility with compile-time type safety.
 */
class StageState
{
  private:
    std::unordered_map<std::string, std::any> fields;

  public:
    StageState() = default;

    /** Set a named field, creating or overwriting it. */
    template <typename T>
    void
    set(const std::string &name, T value)
    {
        fields[name] = std::move(value);
    }

    /**
     * Get a named field by reference.
     *   throws std::out_of_range  if the field is absent
     *   throws std::bad_any_cast  if the field exists but has type != T
     */
    template <typename T>
    const T &
    get(const std::string &name) const
    {
        return std::any_cast<const T &>(fields.at(name));
    }

    /** True if the named field exists (regardless of type). */
    bool
    has(const std::string &name) const
    {
        return fields.find(name) != fields.end();
    }

    /** Remove a single field. No-op if absent. */
    void
    erase(const std::string &name)
    {
        fields.erase(name);
    }

    /** Remove every field. */
    void
    clear()
    {
        fields.clear();
    }

    /** Number of fields currently stored. */
    std::size_t
    size() const
    {
        return fields.size();
    }
};


/**
 * A Stage is a pipeline cycle boundary. It holds two StageState
 * snapshots:
 *
 *   stateIn   Snapshot taken at the start of the current tick.
 *             Treated as read-only by function units during the tick.
 *             Used as the anchor for squash rollback.
 *
 *   stateOut  Snapshot committed at the end of the current tick.
 *             Each tick it starts as a copy of stateIn; function units
 *             running in declared order mutate it.
 *
 * Self-loop between ticks: next tick's stateIn = previous tick's
 * stateOut.  beginTick() performs that promotion.
 *
 * On squash: stateOut reverts to stateIn (this tick makes no
 * progress).  FU-internal persistent state is NOT touched here -- the
 * owning FU handles its own squash.
 */
class Stage
{
  protected:
    std::string stageName;

    StageState stateIn;
    StageState stateOut;

  public:
    explicit
    Stage(const std::string &name)
        : stageName(name)
    {}

    virtual ~Stage() = default;

    const std::string &name() const { return stageName; }

    /** Read-only view of this tick's input snapshot. */
    const StageState &getStateIn() const { return stateIn; }

    /** Read-only view of this tick's working / committed output. */
    const StageState &getStateOut() const { return stateOut; }

    /** Mutable access to stateOut. FUs write here during the tick. */
    StageState &getStateOut() { return stateOut; }

    /**
     * Called at the start of every tick.
     *
     * Promotes the previous tick's stateOut into this tick's stateIn
     * (the self-loop), and seeds stateOut equal to stateIn so the
     * stage's FUs can mutate it in declared order.
     */
    void
    beginTick()
    {
        stateIn = stateOut;
        // stateOut already equals stateIn after the line above, since
        // stateOut was last tick's output. FUs now mutate stateOut.
    }

    /**
     * Called on squash. Drops this tick's work by reverting stateOut
     * to stateIn; the stage makes no forward progress.
     */
    void
    squash()
    {
        stateOut = stateIn;
    }
};

} // namespace gem5

#endif // __CPU_LEGO_STAGE_HH__
