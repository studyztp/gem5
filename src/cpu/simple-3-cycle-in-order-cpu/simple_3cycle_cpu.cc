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

#include "cpu/simple-3-cycle-in-order-cpu/simple_3cycle_cpu.hh"

#include <memory>

#include "arch/generic/pcstate.hh"
#include "base/logging.hh"
#include "cpu/simple-3-cycle-in-order-cpu/lsq.hh"
#include "cpu/simple-3-cycle-in-order-cpu/pfu.hh"
#include "cpu/simple-3-cycle-in-order-cpu/pipeline.hh"
#include "debug/Simple3CPU.hh"
#include "debug/Simple3CPUResp.hh"
#include "mem/packet.hh"
#include "mem/packet_access.hh"
#include "mem/request.hh"
#include "sim/full_system.hh"
#include "sim/system.hh"

namespace gem5
{
namespace simple3
{

Simple3CycleCPU::Simple3CycleCPU(const Params &params)
    : BaseCPU(params),
      icachePort(name() + ".icache_port", *this),
      dcachePort(name() + ".dcache_port", *this),
      simple3Stats(this),
      _haltAddr(params.halt_addr)
{
    fatal_if(numThreads != 1,
             "Simple3CycleCPU only supports a single thread");

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

    lsq = std::make_unique<LSQ>(*this);
    pipeline = std::make_unique<Pipeline>(*this);
}

Simple3CycleCPU::~Simple3CycleCPU()
{
    for (auto *t : threads)
        delete t;
}

void
Simple3CycleCPU::init()
{
    BaseCPU::init();
}

void
Simple3CycleCPU::startup()
{
    BaseCPU::startup();
    DPRINTF(Simple3CPU, "startup\n");

    const auto &p = params();

    // Phase-B synthetic test stream: prepopulate the PFU FIFO with
    // a sequence of fetch words.  Phase B exercises decode + execute
    // without involving the icache port.
    if (p.phase_b_num_words > 0) {
        DPRINTF(Simple3CPU,
                "Phase-B prepopulate: %u words starting at %#x\n",
                p.phase_b_num_words, p.phase_b_start_pc);
        pipeline->prepopulateFifoForPhaseB(
            p.phase_b_start_pc, p.phase_b_num_words, p.phase_b_word_value);
        setThreadStartPc(p.phase_b_start_pc);
    }

    // Phase-C synthetic test stream: write a NOP slide into system
    // memory at phase_c_start_pc via functional system-port access,
    // and reset the PFU's fetchPc to that address.  Real fetches
    // then flow over icachePort during the run.
    if (p.phase_c_num_words > 0) {
        DPRINTF(Simple3CPU,
                "Phase-C memory init: %u words starting at %#x "
                "(value %#x)\n",
                p.phase_c_num_words, p.phase_c_start_pc,
                p.phase_c_word_value);
        for (unsigned i = 0; i < p.phase_c_num_words; ++i) {
            uint32_t word = p.phase_c_word_value;
            const Addr addr = p.phase_c_start_pc + i * sizeof(uint32_t);
            RequestPtr req = std::make_shared<Request>(
                addr, sizeof(uint32_t), 0, instRequestorId());
            PacketPtr pkt = new Packet(req, MemCmd::WriteReq);
            pkt->dataStatic(reinterpret_cast<uint8_t *>(&word));
            system->getSystemPort().sendFunctional(pkt);
            delete pkt;
        }
        pipeline->resetForPhaseC(p.phase_c_start_pc);
        setThreadStartPc(p.phase_c_start_pc);
    }

    // Phase-D synthetic program: vector of 32-bit fetch words
    // (e.g. a hand-coded Thumb-2 kernel) written into memory at
    // phase_d_start_pc.  Differs from Phase C only in that each
    // word can have a distinct value.
    if (!p.phase_d_words.empty()) {
        const Addr entry_pc = p.phase_d_entry_pc != 0
                                  ? p.phase_d_entry_pc
                                  : p.phase_d_start_pc;
        DPRINTF(Simple3CPU,
                "Phase-D memory init: %u words at %#x, entry pc %#x\n",
                (unsigned)p.phase_d_words.size(),
                p.phase_d_start_pc, entry_pc);
        for (size_t i = 0; i < p.phase_d_words.size(); ++i) {
            uint32_t word = p.phase_d_words[i];
            const Addr addr = p.phase_d_start_pc + i * sizeof(uint32_t);
            RequestPtr req = std::make_shared<Request>(
                addr, sizeof(uint32_t), 0, instRequestorId());
            PacketPtr pkt = new Packet(req, MemCmd::WriteReq);
            pkt->dataStatic(reinterpret_cast<uint8_t *>(&word));
            system->getSystemPort().sendFunctional(pkt);
            delete pkt;
        }
        pipeline->resetForPhaseC(entry_pc);
        setThreadStartPc(entry_pc);
    }
}

void
Simple3CycleCPU::setThreadStartPc(Addr startPc)
{
    // Override the workload's reset PC (or whatever SimpleThread
    // initialised pcState to) so Decode and Execute see the test's
    // intended entry point.  Cloning preserves ISA-specific bits
    // (Thumb mode for M-profile, etc).
    SimpleThread *t = thread(0);
    std::unique_ptr<PCStateBase> newPc(t->pcState().clone());
    newPc->set(startPc);
    t->pcState(*newPc);
    DPRINTF(Simple3CPU, "thread pc set to %#x\n", startPc);
}

void
Simple3CycleCPU::regStats()
{
    BaseCPU::regStats();
    pipeline->regStats();
}

void
Simple3CycleCPU::wakeup(ThreadID tid)
{
    DPRINTF(Simple3CPU, "wakeup tid=%d\n", tid);
}

void
Simple3CycleCPU::activateContext(ThreadID thread_id)
{
    DPRINTF(Simple3CPU, "activateContext tid=%d\n", thread_id);
    BaseCPU::activateContext(thread_id);
    pipeline->start();
}

void
Simple3CycleCPU::suspendContext(ThreadID thread_id)
{
    DPRINTF(Simple3CPU, "suspendContext tid=%d\n", thread_id);
    pipeline->stop();
    BaseCPU::suspendContext(thread_id);
}

Counter
Simple3CycleCPU::totalInsts() const
{
    return simple3Stats.instsCommitted.value();
}

Counter
Simple3CycleCPU::totalOps() const
{
    return totalInsts();
}

bool
Simple3CycleCPU::IcachePort::recvTimingResp(PacketPtr pkt)
{
    const Addr addr = pkt->getAddr();
    const uint32_t streamId = pkt->req->streamId();

    cpu.simple3Stats.responsesReceived++;

    // Stream check: a response from an old (squashed) stream is
    // dropped — the FIFO has already been redirected past it and
    // the bytes are wrong-path.
    const bool sameStream = (streamId == cpu.currentStreamId);
    if (!sameStream) {
        DPRINTF(Simple3CPUResp,
                "drop stale response addr=%#x streamId=%u "
                "(current=%u)\n",
                addr, streamId, cpu.currentStreamId);
        cpu.simple3Stats.staleResponsesDropped++;
    } else {
        FetchWord w;
        w.addr = addr;
        w.data = pkt->getLE<uint32_t>();
        w.streamId = streamId;
        cpu.pipeline->getPfu().responseStaging.push_back(w);
        DPRINTF(Simple3CPUResp,
                "stage response addr=%#x data=%#08x streamId=%u\n",
                addr, w.data, streamId);
    }

    cpu.pipeline->getPfu().noteResponseArrived();
    delete pkt;
    return true;
}

void
Simple3CycleCPU::IcachePort::recvReqRetry()
{
    DPRINTF(Simple3CPUResp, "recvReqRetry — re-issuing pending fetch\n");
    cpu.pipeline->retrySendPendingFetch();
}

bool
Simple3CycleCPU::DcachePort::recvTimingResp(PacketPtr pkt)
{
    cpu.lsq->completeRequest(pkt);
    // Note: the packet is owned by the LSQ now and is freed when
    // Execute calls release() after completeAcc().
    return true;
}

void
Simple3CycleCPU::DcachePort::recvReqRetry()
{
    cpu.lsq->retrySend();
}

} // namespace simple3
} // namespace gem5
