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

#include "cpu/signal-3-stage-in-order-cpu/signal_cpu.hh"

#include <memory>

#include "arch/generic/pcstate.hh"
#include "base/logging.hh"
#include "cpu/signal-3-stage-in-order-cpu/fetch.hh"
#include "cpu/signal-3-stage-in-order-cpu/lsq.hh"
#include "cpu/signal-3-stage-in-order-cpu/pipeline.hh"
#include "debug/Signal3CPU.hh"
#include "debug/Signal3CPUMem.hh"
#include "mem/packet.hh"
#include "mem/packet_access.hh"
#include "mem/request.hh"
#include "sim/full_system.hh"
#include "sim/system.hh"

namespace gem5
{
namespace signal3
{

SignalCPU::SignalCPU(const Params &params)
    : BaseCPU(params),
      icachePort(name() + ".icache_port", *this),
      dcachePort(name() + ".dcache_port", *this),
      _haltAddr(params.halt_addr),
      _stats(this)
{
    fatal_if(numThreads != 1,
             "SignalCPU only supports a single thread");

    for (ThreadID i = 0; i < numThreads; i++) {
        SimpleThread *t;
        if (FullSystem) {
            t = new SimpleThread(this, i, params.system, params.mmu,
                                 params.isa[i], params.decoder[i]);
            t->setStatus(ThreadContext::Halted);
        } else {
            t = new SimpleThread(this, i, params.system,
                                 params.workload[i], params.mmu,
                                 params.isa[i], params.decoder[i]);
        }
        threads.push_back(t);
        threadContexts.push_back(t->getTC());
    }

    _pipeline = std::make_unique<Pipeline>(*this, params.pfu_fifo_words);
}

SignalCPU::~SignalCPU()
{
    for (auto *t : threads)
        delete t;
}

void
SignalCPU::init()
{
    BaseCPU::init();
}

void
SignalCPU::startup()
{
    BaseCPU::startup();
    DPRINTF(Signal3CPU, "startup\n");

    // Run real STM32G4 firmware: the workload (loaded by
    // ArmMFsWorkload) has placed the binary in Flash and SRAM and
    // initialised the thread's reset PC from the ELF entry symbol
    // (Reset_Handler).  All we do here is seed the pipeline's
    // fetchPc to that same address so F starts pulling instructions
    // from the reset vector.
    const Addr resetPc = thread(0)->pcState().instAddr();
    DPRINTF(Signal3CPU,
            "seeding pipeline from workload reset PC %#x\n", resetPc);
    _pipeline->resetTo(resetPc);
}

void
SignalCPU::regStats()
{
    BaseCPU::regStats();
    _pipeline->regStats();   // registers Ticked::numCycles
}

void
SignalCPU::wakeup(ThreadID tid)
{
    DPRINTF(Signal3CPU, "wakeup tid=%d\n", tid);
}

void
SignalCPU::activateContext(ThreadID thread_id)
{
    DPRINTF(Signal3CPU, "activateContext tid=%d\n", thread_id);
    BaseCPU::activateContext(thread_id);
    _pipeline->start();
}

void
SignalCPU::suspendContext(ThreadID thread_id)
{
    DPRINTF(Signal3CPU, "suspendContext tid=%d\n", thread_id);
    _pipeline->stop();
    BaseCPU::suspendContext(thread_id);
}

Counter
SignalCPU::totalInsts() const
{
    return _stats.instsCommitted.value();
}

Counter
SignalCPU::totalOps() const
{
    return totalInsts();
}

bool
SignalCPU::IcachePort::recvTimingResp(PacketPtr pkt)
{
    const Addr addr = pkt->getAddr();
    const uint32_t streamId = pkt->req->streamId();

    cpu._stats.responsesReceived++;

    const bool sameStream = (streamId == cpu.currentStreamId());
    if (!sameStream) {
        DPRINTF(Signal3CPUMem,
                "drop stale response addr=%#x streamId=%u (cur=%u)\n",
                addr, streamId, cpu.currentStreamId());
        cpu._stats.staleResponsesDropped++;
    } else {
        FetchWord w;
        w.addr = addr;
        w.data = pkt->getLE<uint32_t>();
        w.streamId = streamId;
        cpu.pipeline().onAsyncResponse(w, PortSide::Icache);
    }

    cpu.pipeline().getFetch().noteResponseArrived();
    delete pkt;
    return true;
}

void
SignalCPU::IcachePort::recvReqRetry()
{
    DPRINTF(Signal3CPUMem, "icache recvReqRetry\n");
    cpu.pipeline().retrySendPendingFetch();
}

bool
SignalCPU::DcachePort::recvTimingResp(PacketPtr pkt)
{
    cpu.pipeline().onDcacheResponse(pkt);
    // pkt ownership: held by LSQ until E calls release() (which
    // deletes it).  Don't delete here.
    return true;
}

void
SignalCPU::DcachePort::recvReqRetry()
{
    cpu.pipeline().retrySendPendingDcache();
}

LSQ &
SignalCPU::getLsq()
{
    return _pipeline->getLsq();
}

} // namespace signal3
} // namespace gem5
