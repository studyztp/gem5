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

#ifndef __CPU_SIMPLE_3_CYCLE_IN_ORDER_CPU_SIMPLE_3CYCLE_CPU_HH__
#define __CPU_SIMPLE_3_CYCLE_IN_ORDER_CPU_SIMPLE_3CYCLE_CPU_HH__

#include <memory>
#include <vector>

#include "cpu/base.hh"
#include "cpu/simple-3-cycle-in-order-cpu/stats.hh"
#include "cpu/simple_thread.hh"
#include "params/Simple3CycleCPU.hh"

namespace gem5
{
namespace simple3
{

class Pipeline;
class LSQ;

/**
 * Top-level SimObject for the Simple 3-Cycle In-Order CPU.
 *
 * ISA-agnostic — uses only the generic ThreadContext / InstDecoder /
 * StaticInst interfaces.  Flash + AHB timing is delegated to a
 * PipelinedSimpleMemory connected to the icachePort; this CPU does
 * not model Flash internally.
 */
class Simple3CycleCPU : public BaseCPU
{
  protected:
    /** Instruction-side AHB master port.  Tags every outgoing
     *  request with currentStreamId for the AHB-retraction protocol. */
    class IcachePort : public RequestPort
    {
      public:
        Simple3CycleCPU &cpu;

        IcachePort(const std::string &name_, Simple3CycleCPU &cpu_)
            : RequestPort(name_), cpu(cpu_)
        {}

      protected:
        bool recvTimingResp(PacketPtr pkt) override;
        void recvReqRetry() override;
    };

    /** Stub data port — Phase 1 has no data accesses but BaseCPU
     *  symmetry requires getDataPort() to return something. */
    class DcachePort : public RequestPort
    {
      public:
        Simple3CycleCPU &cpu;

        DcachePort(const std::string &name_, Simple3CycleCPU &cpu_)
            : RequestPort(name_), cpu(cpu_)
        {}

      protected:
        bool recvTimingResp(PacketPtr pkt) override;
        void recvReqRetry() override;
    };

    std::vector<SimpleThread *> threads;
    IcachePort icachePort;
    DcachePort dcachePort;

    Port &getDataPort() override { return dcachePort; }
    Port &getInstPort() override { return icachePort; }

  public:
    PARAMS(Simple3CycleCPU);
    Simple3CycleCPU(const Params &params);
    ~Simple3CycleCPU();

    void init() override;
    void startup() override;
    void regStats() override;

    void wakeup(ThreadID tid) override;
    void activateContext(ThreadID thread_id) override;
    void suspendContext(ThreadID thread_id) override;

    Counter totalInsts() const override;
    Counter totalOps() const override;

    /** Accessors for the stages — friends would also work.
     *  params() is provided by the PARAMS() macro above. */
    SimpleThread *thread(ThreadID tid) const { return threads[tid]; }
    Addr haltAddr() const { return _haltAddr; }
    IcachePort &getIcachePort() { return icachePort; }
    DcachePort &getDcachePort() { return dcachePort; }
    LSQ &getLsq() { return *lsq; }

    /** AHB stream tag for instruction fetches.  Bumped on every
     *  redirect (mispredict in E or backward predict in D). */
    uint32_t currentStreamId = 1;

    /** Per-CPU stats group. */
    Simple3Stats simple3Stats;

  private:
    Addr _haltAddr;
    std::unique_ptr<Pipeline> pipeline;
    std::unique_ptr<LSQ> lsq;

    /** Clone the current thread's PCStateBase, override its address
     *  to startPc, and write back — keeps ISA-specific bits like
     *  Thumb mode intact. */
    void setThreadStartPc(Addr startPc);
};

} // namespace simple3
} // namespace gem5

#endif // __CPU_SIMPLE_3_CYCLE_IN_ORDER_CPU_SIMPLE_3CYCLE_CPU_HH__
