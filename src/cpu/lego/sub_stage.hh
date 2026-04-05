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

#ifndef __CPU_LEGO_SUB_STAGE_HH__
#define __CPU_LEGO_SUB_STAGE_HH__

#include <vector>

#include "arch/generic/mmu.hh"
#include "params/LegoSubStage.hh"
#include "sim/sim_object.hh"

namespace gem5
{

class SimpleThread;
class Stage;
class StageFunction;

class SubStage : public SimObject
{
  public:
    PARAMS(LegoSubStage);
    SubStage(const Params &params);

    /** Set the parent stage (called by Stage during construction). */
    void setStage(Stage *stage) { _stage = stage; }

    /** Latch cross-stage inputs on all functions. */
    void latchInputs();

    /** Call compute() on all functions in order. */
    void compute();

    /** Flush all functions. */
    void flush();

    /** Get the parent stage. */
    Stage *stage() { return _stage; }

    /** Get a thread from the CPU. */
    SimpleThread *getThread(ThreadID tid);

    /** Propagate needUpdate up to stage. */
    void notifyUpdate();

    /** Check if tick is still within the current cycle. */
    bool isCurrentCycle(Tick tick) const;

    /** Send a packet up the chain to the CPU. */
    void sendPacketToStage(Packet *pkt);

    /** Send a translation request up the chain to the CPU. */
    void sendTranslationToStage(RequestPtr req,
                                BaseMMU::Translation *translation,
                                BaseMMU::Mode mode);

    /** Receive a timing response and dispatch to function. */
    void recvTimingResp(Packet *pkt);

    /** Get function list (for connection wiring). */
    const std::vector<StageFunction *> &getFunctions() const
    { return functions; }

  private:
    Stage *_stage;
    std::vector<StageFunction *> functions;
};

} // namespace gem5

#endif // __CPU_LEGO_SUB_STAGE_HH__
