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

#include "cpu/lego/lego_cpu.hh"

#include "debug/LegoCPU.hh"
#include "sim/full_system.hh"

namespace gem5
{

LegoCPU::LegoCPU(const Params &params)
    : BaseCPU(params),
      icachePort(name() + ".icache_port", *this),
      dcachePort(name() + ".dcache_port", *this)
{
    for (ThreadID i = 0; i < numThreads; i++) {
        SimpleThread *thread;
        if (FullSystem) {
            thread = new SimpleThread(this, i, params.system,
                                      params.mmu, params.isa[i],
                                      params.decoder[i]);
            thread->setStatus(ThreadContext::Halted);
        } else {
            thread = new SimpleThread(this, i, params.system,
                                      params.workload[i], params.mmu,
                                      params.isa[i], params.decoder[i]);
        }
        threads.push_back(thread);
        threadContexts.push_back(thread->getTC());
    }
}

LegoCPU::~LegoCPU()
{
    for (auto *thread : threads) {
        delete thread;
    }
}

void
LegoCPU::init()
{
    BaseCPU::init();
}

void
LegoCPU::startup()
{
    BaseCPU::startup();
}

void
LegoCPU::wakeup(ThreadID tid)
{
}

void
LegoCPU::activateContext(ThreadID thread_id)
{
    BaseCPU::activateContext(thread_id);
}

void
LegoCPU::suspendContext(ThreadID thread_id)
{
    BaseCPU::suspendContext(thread_id);
}

Counter
LegoCPU::totalInsts() const
{
    return 0;
}

Counter
LegoCPU::totalOps() const
{
    return 0;
}

bool
LegoCPU::IcachePort::recvTimingResp(PacketPtr pkt)
{
    return true;
}

void
LegoCPU::IcachePort::recvReqRetry()
{
}

bool
LegoCPU::DcachePort::recvTimingResp(PacketPtr pkt)
{
    return true;
}

void
LegoCPU::DcachePort::recvReqRetry()
{
}

} // namespace gem5
