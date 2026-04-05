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

#ifndef __CPU_LEGO_PORT_HH__
#define __CPU_LEGO_PORT_HH__

#include <string>
#include <typeinfo>
#include <vector>

#include "base/logging.hh"
#include "base/types.hh"
#include "sim/cur_tick.hh"

namespace gem5
{

class StageFunction;

/** Base class for type-erased port references used in connection
 *  wiring and runtime type checking. */
class PortBase
{
  public:
    PortBase(const std::string &name, unsigned stage_id)
        : _name(name), _stageId(stage_id) {}
    virtual ~PortBase() = default;

    const std::string &name() const { return _name; }
    unsigned stageId() const { return _stageId; }
    void setStageId(unsigned id) { _stageId = id; }
    virtual const std::type_info &dataType() const = 0;
    virtual bool isOutput() const = 0;

    /** Type-erased connection. Connects this output to an input.
     *  Fatals if types don't match or if this is not an output. */
    virtual void connectTo(PortBase *input) = 0;

  protected:
    std::string _name;
    unsigned _stageId;
};

template<typename T> class Input;

/**
 * Typed output port. Holds a value and notifies connected
 * consumers when a new value is written.
 */
template<typename T>
class Output : public PortBase
{
  public:
    Output(const std::string &name, unsigned stage_id)
        : PortBase(name, stage_id) {}

    const std::type_info &dataType() const override
    { return typeid(T); }

    bool isOutput() const override { return true; }

    void connectTo(PortBase *input) override
    {
        fatal_if(dataType() != input->dataType(),
                 "Port type mismatch: output '%s' (%s) -> input '%s' (%s)",
                 name(), dataType().name(),
                 input->name(), input->dataType().name());
        auto *typed = static_cast<Input<T>*>(input);
        addConsumer(typed);
        typed->addSource(this);
    }

    /** Write a new value. Notifies all consumers.
     *  No-op if value is unchanged or output is blocked. */
    bool write(const T &v)
    {
        if (blocked)
            return false;
        // Don't trigger if value unchanged
        if (valid && value == v)
            return true;
        value = v;
        valid = true;
        lastWritten = curTick();
        writerStageId = _stageId;
        for (auto *input : consumers)
            input->notify(_stageId, lastWritten);
        return true;
    }

    const T &read() const { return value; }
    bool hasData() const { return valid; }
    bool isBlocked() const { return blocked; }
    Tick getLastWritten() const { return lastWritten; }
    unsigned getWriterStageId() const { return writerStageId; }

    void block() { blocked = true; }
    void unblock() { blocked = false; }

    void clear()
    {
        valid = false;
        blocked = false;
    }

    void addConsumer(Input<T> *input)
    {
        consumers.push_back(input);
    }

  private:
    T value;
    bool valid = false;
    bool blocked = false;
    Tick lastWritten = 0;
    unsigned writerStageId = 0;
    std::vector<Input<T>*> consumers;
};

/**
 * Typed input port. Connected to one or more output ports.
 * When an output writes, this input's notify() is called,
 * which triggers the owning function's compute().
 */
template<typename T>
class Input : public PortBase
{
  public:
    Input(const std::string &name, unsigned stage_id,
          StageFunction *owner)
        : PortBase(name, stage_id), ownerFunc(owner) {}

    const std::type_info &dataType() const override
    { return typeid(T); }

    bool isOutput() const override { return false; }

    void connectTo(PortBase *input) override
    {
        panic("Cannot connect from an input port\n");
    }

    /** Called by Output::write() when new data is available.
     *  Stores the writer's stage ID and write tick. */
    void notify(unsigned writer_stage_id, Tick written_at);

    /** Stage ID of the output that last wrote to this input. */
    unsigned getWriterStageId() const { return lastWriterStageId; }

    /** Tick when the source last wrote. */
    Tick getLastWritten() const { return lastWrittenTick; }

    /** Read the most recent value from any source that has data. */
    const T &read() const
    {
        for (auto *src : sources) {
            if (src->hasData())
                return src->read();
        }
        panic("Input::read() called with no valid source data\n");
    }

    /** Check if any source has data. */
    bool hasData() const
    {
        for (auto *src : sources) {
            if (src->hasData())
                return true;
        }
        return false;
    }

    /** Block all source outputs from writing. */
    void blockSource()
    {
        for (auto *src : sources)
            src->block();
    }

    /** Unblock all source outputs. */
    void unblockSource()
    {
        for (auto *src : sources)
            src->unblock();
    }

    void addSource(Output<T> *output)
    {
        sources.push_back(output);
    }

    void setOwner(StageFunction *owner)
    {
        ownerFunc = owner;
    }

  private:
    std::vector<Output<T>*> sources;
    StageFunction *ownerFunc;
    unsigned lastWriterStageId = 0;
    Tick lastWrittenTick = 0;
};

} // namespace gem5

#endif // __CPU_LEGO_PORT_HH__
