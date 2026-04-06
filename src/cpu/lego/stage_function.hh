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

#ifndef __CPU_LEGO_STAGE_FUNCTION_HH__
#define __CPU_LEGO_STAGE_FUNCTION_HH__

#include <string>
#include <unordered_map>

#include "arch/generic/mmu.hh"
#include "cpu/lego/port.hh"
#include "cpu/lego/type.hh"
#include "mem/packet.hh"
#include "mem/request.hh"
#include "params/LegoStageFunction.hh"
#include "sim/sim_object.hh"

namespace gem5
{

class SubStage;
class Stage;

/**
 * Base class for all pipeline stage functions.
 *
 * Each function declares typed input/output ports.
 * When an input receives new data, compute() is called.
 * compute() reads inputs, performs logic, writes outputs.
 */
class StageFunction : public SimObject
{
  public:
    PARAMS(LegoStageFunction);
    StageFunction(const Params &params);
    virtual ~StageFunction() = default;

    /** Latch cross-stage inputs at the start of each tick().
     *  Copies data from cross-stage source outputs into local
     *  variables. Called by CPU before compute() loop. */
    virtual void latchInputs() {}

    /** Perform this function's computation.
     *  Called by tick() and by reactive notify(). */
    virtual void compute() = 0;

    /** Reset all state (squash). */
    virtual void flush() {}

    /** Return a short trace string for line tracing. */
    virtual std::string traceStatus() const { return ""; }

    /** Set the parent substage. */
    void setSubStage(SubStage *sub) { _subStage = sub; }

    /** Set the stage ID (for timing decisions in ports). */
    void setStageId(unsigned id);

    unsigned getStageId() const { return stageId; }

    const std::string &getFuncName() const { return funcName; }

    /** Called by Input::notify() for cross-stage data.
     *  Propagates needUpdate up the chain. */
    void notifyUpdate();

    /** Check if tick is still within the current cycle. */
    bool isCurrentCycle(Tick tick) const;

    /** Register a port by name for connection wiring. */
    void registerPort(const std::string &name, PortBase *port);

    /** Look up a port by name. Returns nullptr if not found. */
    PortBase *findPort(const std::string &name) const;

    /** Get all registered ports. */
    const std::unordered_map<std::string, PortBase *> &
    getPorts() const { return portMap; }

    /** Wire this function's output port to a consumer's input port
     *  with the same name. */
    void wireOutput(const std::string &portName,
                    StageFunction *consumer);

    /** Wire a producer's output port to this function's input port
     *  with the same name. */
    void wireInput(const std::string &portName,
                   StageFunction *producer);

  protected:
    SubStage *_subStage = nullptr;
    unsigned stageId = 0;
    std::string funcName;

  private:
    std::unordered_map<std::string, PortBase *> portMap;
};

/**
 * Stage function that communicates via memory ports (icache/dcache).
 *
 * Uses FuncSenderState to track the send/recv chain.
 */
class StageFunctionWithPort : public StageFunction
{
  public:
    using StageFunction::StageFunction;  // inherit constructor
    virtual ~StageFunctionWithPort() = default;

    /** SenderState pushed onto packets for response routing. */
    struct FuncSenderState : public Packet::SenderState
    {
        StageFunctionWithPort *func;
        SubStage *subStage;
        Stage *stage;
        InstSeqNum instSeqNum;
        PortType portType;
        FuncSenderState(StageFunctionWithPort *f,
                        const InstSeqNum &inst_seq,
                        PortType port_type)
            : func(f), subStage(nullptr), stage(nullptr),
              instSeqNum(inst_seq), portType(port_type) {}
    };

    /** Called when a timing response arrives for this function. */
    virtual void recvTimingResp(PacketPtr pkt) {}
};

/**
 * Stage function that uses MMU translation (via composition).
 *
 * Contains a TranslationCallback object that implements
 * BaseMMU::Translation. When translation completes, it calls
 * translationComplete() on this function.
 */
class StageFunctionTranslation : public StageFunction
{
  public:
    using Params = LegoStageFunctionParams;
    StageFunctionTranslation(const Params &params);
    virtual ~StageFunctionTranslation() = default;

  protected:
    /** Called when MMU translation completes. */
    virtual void translationComplete(const Fault &fault,
        const RequestPtr &req, ThreadContext *tc,
        BaseMMU::Mode mode) = 0;

    /** The request for the current translation. */
    RequestPtr translationReq;

    /** Get the callback object to pass to translateTiming(). */
    BaseMMU::Translation *getTranslationCallback()
    { return &translationCB; }

  private:
    class TranslationCallback : public BaseMMU::Translation
    {
        StageFunctionTranslation &owner;
      public:
        TranslationCallback(StageFunctionTranslation &o)
            : owner(o) {}
        void markDelayed() override {}
        void finish(const Fault &fault, const RequestPtr &req,
                    ThreadContext *tc, BaseMMU::Mode mode) override
        {
            owner.translationComplete(fault, req, tc, mode);
        }
    };

    TranslationCallback translationCB{*this};
};

// --- Input::notify() implementation ---
// Defined here because it needs the StageFunction definition.
template<typename T>
void
Input<T>::notify(unsigned writer_stage_id, Tick written_at)
{
    if (!ownerFunc)
        return;

    lastWriterStageId = writer_stage_id;
    lastWrittenTick = written_at;

    if (writer_stage_id == _stageId) {
        ownerFunc->compute();
    } else {
        ownerFunc->notifyUpdate();
    }
}

// --- Input::notifyDiscard() implementation ---
// Discard calls flush() which clears state and propagates.
template<typename T>
void
Input<T>::notifyDiscard(unsigned writer_stage_id, Tick written_at)
{
    if (!ownerFunc)
        return;

    lastWriterStageId = writer_stage_id;
    lastWrittenTick = written_at;

    // Flush clears local state and propagates discard to output
    ownerFunc->flush();
}

} // namespace gem5

#endif // __CPU_LEGO_STAGE_FUNCTION_HH__
