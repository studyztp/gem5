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

#ifndef __CPU_LEGO_LEGO_CPU_HH__
#define __CPU_LEGO_LEGO_CPU_HH__

#include <vector>

#include "cpu/base.hh"
#include "cpu/simple_thread.hh"
#include "params/LegoCPU.hh"

namespace gem5
{

class LegoCPU : public BaseCPU
{
  protected:
    std::vector<SimpleThread *> threads;

    class IcachePort : public RequestPort
    {
      public:
        LegoCPU &cpu;

        IcachePort(const std::string &name, LegoCPU &cpu_)
            : RequestPort(name), cpu(cpu_)
        {}

      protected:
        bool recvTimingResp(PacketPtr pkt) override;
        void recvReqRetry() override;
    };

    class DcachePort : public RequestPort
    {
      public:
        LegoCPU &cpu;

        DcachePort(const std::string &name, LegoCPU &cpu_)
            : RequestPort(name), cpu(cpu_)
        {}

      protected:
        bool recvTimingResp(PacketPtr pkt) override;
        void recvReqRetry() override;
    };

    IcachePort icachePort;
    DcachePort dcachePort;

    Port &getDataPort() override { return dcachePort; }
    Port &getInstPort() override { return icachePort; }

  public:
    PARAMS(LegoCPU);
    LegoCPU(const Params &params);
    ~LegoCPU();

    void init() override;
    void startup() override;

    void wakeup(ThreadID tid) override;

    void activateContext(ThreadID thread_id) override;
    void suspendContext(ThreadID thread_id) override;

    Counter totalInsts() const override;
    Counter totalOps() const override;
};

} // namespace gem5

#endif // __CPU_LEGO_LEGO_CPU_HH__
