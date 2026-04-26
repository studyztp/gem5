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

#ifndef __CPU_SIGNAL_3_STAGE_IN_ORDER_CPU_SIGNAL_CPU_HH__
#define __CPU_SIGNAL_3_STAGE_IN_ORDER_CPU_SIGNAL_CPU_HH__

#include <memory>
#include <vector>

#include "cpu/base.hh"
#include "cpu/signal-3-stage-in-order-cpu/stats.hh"
#include "cpu/simple_thread.hh"
#include "params/SignalCPU.hh"

namespace gem5
{
namespace signal3
{

class Pipeline;

/**
 * Top-level SimObject for the signal-driven 3-stage in-order CPU.
 *
 * Mostly mirrors Simple3CycleCPU but routes per-cycle work through
 * the Signal/Latch graph in Pipeline.  ISA-agnostic — uses generic
 * ThreadContext / InstDecoder / StaticInst interfaces.
 */
class SignalCPU : public BaseCPU
{
  protected:
    class IcachePort : public RequestPort
    {
      public:
        SignalCPU &cpu;

        IcachePort(const std::string &name_, SignalCPU &cpu_)
            : RequestPort(name_), cpu(cpu_)
        {}

      protected:
        bool recvTimingResp(PacketPtr pkt) override;
        void recvReqRetry() override;
    };

    class DcachePort : public RequestPort
    {
      public:
        SignalCPU &cpu;

        DcachePort(const std::string &name_, SignalCPU &cpu_)
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
    PARAMS(SignalCPU);
    SignalCPU(const Params &params);
    ~SignalCPU();

    void init() override;
    void startup() override;
    void regStats() override;

    void wakeup(ThreadID tid) override;
    void activateContext(ThreadID thread_id) override;
    void suspendContext(ThreadID thread_id) override;

    Counter totalInsts() const override;
    Counter totalOps() const override;

    SimpleThread *thread(ThreadID tid) const { return threads[tid]; }
    Addr haltAddr() const { return _haltAddr; }
    IcachePort &getIcachePort() { return icachePort; }
    DcachePort &getDcachePort() { return dcachePort; }
    class LSQ &getLsq();

    /** AHB stream tag for instruction fetches.  Bumped on every
     *  redirect.  S1 keeps it constant. */
    uint32_t currentStreamId() const { return _currentStreamId; }
    void bumpStreamId() { ++_currentStreamId; }

    SignalCPUStats &signalStats() { return _stats; }

    /** Used by ports' recvTimingResp to forward to Pipeline. */
    Pipeline &pipeline() { return *_pipeline; }

  private:
    Addr _haltAddr;
    uint32_t _currentStreamId = 1;
    std::unique_ptr<Pipeline> _pipeline;
    SignalCPUStats _stats;
};

} // namespace signal3
} // namespace gem5

#endif // __CPU_SIGNAL_3_STAGE_IN_ORDER_CPU_SIGNAL_CPU_HH__
