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

#include "cpu/lego/stage.hh"
#include "cpu/lego/stage_function.hh"
#include "cpu/lego/sub_stage.hh"
#include "debug/LegoCPU.hh"
#include "debug/LegoCPUFunc.hh"
#include "debug/LegoCPULineTrace.hh"
#include "sim/full_system.hh"

namespace gem5
{

LegoCPU::LegoCPU(const Params &params)
    : BaseCPU(params),
      icachePort(name() + ".icache_port", *this),
      dcachePort(name() + ".dcache_port", *this),
      tickEvent([this] { tick(); }, name() + ".tick", false,
                Event::CPU_Tick_Pri),
      updateEvent([this] { update(); }, name() + ".update", false,
                  Event::CPU_Tick_Pri)
{
    // Create threads
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

    // Build pipeline from Python config
    for (unsigned i = 0; i < params.stages.size(); i++) {
        Stage *stage = params.stages[i];
        stage->setCPU(this, i);
        stages.push_back(stage);
    }

    // Wire port connections
    wireConnections();
}

LegoCPU::~LegoCPU()
{
    for (auto *thread : threads)
        delete thread;
}

void
LegoCPU::init()
{
    BaseCPU::init();

    if (!params().switched_out &&
        system->getMemoryMode() != enums::timing) {
        fatal("The Lego CPU requires the memory system to be in "
              "'timing' mode.\n");
    }
}

void
LegoCPU::startup()
{
    BaseCPU::startup();

    // Print line trace header
    if (debug::LegoCPULineTrace) {
        std::string header = csprintf("%-14s |", "Cycle");
        for (unsigned i = 0; i < stages.size(); i++)
            header += csprintf(" %-16s |", csprintf("S%d", i));
        DPRINTF(LegoCPULineTrace, "%s\n", header);
    }
}

void
LegoCPU::wakeup(ThreadID tid)
{
    DPRINTF(LegoCPU, "Wakeup thread %d\n", tid);
    assert(tid < numThreads);

    if (threads[tid]->status() == ThreadContext::Suspended)
        threads[tid]->activate();
}

void
LegoCPU::activateContext(ThreadID thread_id)
{
    DPRINTF(LegoCPU, "ActivateContext thread: %d\n", thread_id);

    threads[thread_id]->activate();

    if (!tickEvent.scheduled())
        schedule(tickEvent, clockEdge(Cycles(0)));

    BaseCPU::activateContext(thread_id);
}

void
LegoCPU::suspendContext(ThreadID thread_id)
{
    DPRINTF(LegoCPU, "SuspendContext thread: %d\n", thread_id);

    threads[thread_id]->suspend();

    if (tickEvent.scheduled())
        deschedule(tickEvent);
    if (updateEvent.scheduled())
        deschedule(updateEvent);

    BaseCPU::suspendContext(thread_id);
}

Counter
LegoCPU::totalInsts() const
{
    Counter ret = 0;
    for (auto *thread : threads)
        ret += thread->numInst;
    return ret;
}

Counter
LegoCPU::totalOps() const
{
    Counter ret = 0;
    for (auto *thread : threads)
        ret += thread->numOp;
    return ret;
}

void
LegoCPU::serializeThread(CheckpointOut &cp, ThreadID tid) const
{
    threads[tid]->serialize(cp);
}

void
LegoCPU::unserializeThread(CheckpointIn &cp, ThreadID tid)
{
    threads[tid]->unserialize(cp);
}

DrainState
LegoCPU::drain()
{
    if (switchedOut())
        return DrainState::Drained;

    if (tickEvent.scheduled())
        deschedule(tickEvent);
    if (updateEvent.scheduled())
        deschedule(updateEvent);

    DPRINTF(LegoCPU, "LegoCPU drained\n");
    return DrainState::Drained;
}

void
LegoCPU::drainResume()
{
    if (switchedOut())
        return;

    DPRINTF(LegoCPU, "LegoCPU drainResume\n");

    if (!system->isTimingMode()) {
        fatal("The Lego CPU requires the memory system to be in "
              "'timing' mode.\n");
    }

    for (ThreadID tid = 0; tid < numThreads; tid++)
        wakeup(tid);
}

void
LegoCPU::switchOut()
{
    assert(!switchedOut());
    BaseCPU::switchOut();

    if (tickEvent.scheduled())
        deschedule(tickEvent);
    if (updateEvent.scheduled())
        deschedule(updateEvent);
}

void
LegoCPU::takeOverFrom(BaseCPU *old_cpu)
{
    BaseCPU::takeOverFrom(old_cpu);
}

void
LegoCPU::verifyMemoryMode() const
{
    if (!system->isTimingMode()) {
        fatal("The Lego CPU requires the memory system to be in "
              "'timing' mode.\n");
    }
}

// --------- Connection Wiring ---------

void
LegoCPU::wireConnections()
{
    // Build funcName → StageFunction* map
    std::unordered_map<std::string, StageFunction *> funcMap;
    for (auto *stage : stages) {
        for (auto *sub : stage->getSubStages()) {
            for (auto *func : sub->getFunctions()) {
                std::string fn = func->getFuncName();
                fatal_if(fn.empty(),
                    "Function %s has no funcName set\n",
                    func->name());
                fatal_if(funcMap.count(fn),
                    "Duplicate funcName '%s'\n", fn);
                funcMap[fn] = func;
            }
        }
    }

    // Parse connection strings: "srcFunc.port:dstFunc.port"
    for (const auto &conn : params().connections) {
        auto colon = conn.find(':');
        fatal_if(colon == std::string::npos,
            "Invalid connection format '%s' (expected src.port:dst.port)\n",
            conn);

        std::string srcStr = conn.substr(0, colon);
        std::string dstStr = conn.substr(colon + 1);

        auto srcDot = srcStr.find('.');
        auto dstDot = dstStr.find('.');
        fatal_if(srcDot == std::string::npos ||
                 dstDot == std::string::npos,
            "Invalid connection format '%s'\n", conn);

        std::string srcFunc = srcStr.substr(0, srcDot);
        std::string srcPort = srcStr.substr(srcDot + 1);
        std::string dstFunc = dstStr.substr(0, dstDot);
        std::string dstPort = dstStr.substr(dstDot + 1);

        auto srcIt = funcMap.find(srcFunc);
        auto dstIt = funcMap.find(dstFunc);
        fatal_if(srcIt == funcMap.end(),
            "Connection '%s': unknown function '%s'\n",
            conn, srcFunc);
        fatal_if(dstIt == funcMap.end(),
            "Connection '%s': unknown function '%s'\n",
            conn, dstFunc);

        srcIt->second->wireOutput(srcPort, dstIt->second);

        DPRINTF(LegoCPU, "Wired %s.%s -> %s.%s (stage %d -> %d)\n",
                srcFunc, srcPort, dstFunc, dstPort,
                srcIt->second->getStageId(),
                dstIt->second->getStageId());
    }

    // Print full port graph
    DPRINTF(LegoCPU, "=== Port Graph ===\n");
    for (auto &[fn, func] : funcMap) {
        for (auto &[portName, port] : func->getPorts()) {
            DPRINTF(LegoCPU, "  [%s] %s.%s (stage %d, type %s)\n",
                    port->isOutput() ? "OUT" : "IN",
                    fn, portName,
                    port->stageId(),
                    port->dataType().name());
        }
    }
    DPRINTF(LegoCPU, "=== End Port Graph ===\n");
}

// --------- Pipeline Tick ---------

void
LegoCPU::tick()
{
    cycleStartTick = curTick();
    needUpdate = false;

    // Calculate current cycle from tick
    uint64_t newCycle = curTick() / clockPeriod() + 1;

    // Print stall cycles we missed (no tick was scheduled).
    // Use lastTraceStatus which was captured at end of last tick.
    if (debug::LegoCPULineTrace && newCycle > curCycle + 1) {
        for (uint64_t c = curCycle + 1; c < newCycle; c++) {
            DPRINTF(LegoCPULineTrace, "%s\n",
                    csprintf("Cycle %6d |%s (stall)", c,
                             lastTraceLine));
        }
    }

    curCycle = newCycle;

    DPRINTF(LegoCPU, "Tick cycle %d\n", curCycle);

    // Latch cross-stage inputs at cycle boundary
    for (auto *stage : stages)
        stage->latchInputs();

    // Compute all stages
    for (auto *stage : stages)
        stage->compute();

    updateCycleCounters(BaseCPU::CPU_STATE_ON);

    // Reschedule if any cross-stage output was written this cycle
    if (needUpdate && !tickEvent.scheduled())
        schedule(tickEvent, clockEdge(Cycles(1)));

    if (debug::LegoCPULineTrace) {
        printLineTrace();
        lastTraceLine = "";
        for (auto *stage : stages)
            lastTraceLine += csprintf(" %-16s |", stage->traceStatus());
    }
}

void
LegoCPU::notifyUpdate()
{
    needUpdate = true;

    // An async response wrote a cross-stage output. Schedule
    // update event to ensure tick fires next cycle.
    if (!updateEvent.scheduled())
        schedule(updateEvent, curTick());
}

void
LegoCPU::update()
{
    // Async response arrived mid-cycle. The data is already
    // written to the output port. Schedule tick for next cycle
    // so latchInputs() delivers it to the consuming stage.
    if (needUpdate && !tickEvent.scheduled())
        schedule(tickEvent, clockEdge(Cycles(1)));
}

void
LegoCPU::printLineTrace()
{
    std::string line = csprintf("Cycle %6d |", curCycle);
    for (auto *stage : stages)
        line += csprintf(" %-16s |", stage->traceStatus());
    DPRINTF(LegoCPULineTrace, "%s\n", line);
}

// --------- Send path ---------

void
LegoCPU::sendPacketToPort(Packet *pkt)
{
    auto *ss = safe_cast<StageFunctionWithPort::FuncSenderState *>(
        pkt->senderState);

    switch (ss->portType) {
      case ICACHE:
        DPRINTF(LegoCPU, "Sending packet to icachePort\n");
        if (!icachePort.sendTimingReq(pkt)) {
            // TODO: handle retry — save packet for recvReqRetry
        }
        break;
      case DCACHE:
        DPRINTF(LegoCPU, "Sending packet to dcachePort\n");
        if (!dcachePort.sendTimingReq(pkt)) {
            // TODO: handle retry — save packet for recvReqRetry
        }
        break;
      default:
        panic("Unknown PortType in sendPacketToPort\n");
    }
}

void
LegoCPU::sendTranslationToMMU(RequestPtr req,
                              BaseMMU::Translation *translation,
                              BaseMMU::Mode mode)
{
    ThreadContext *tc = getContext(0);
    req->setContext(tc->contextId());
    tc->getMMUPtr()->translateTiming(req, tc, translation, mode);
}

// --------- Recv path ---------

void
LegoCPU::recvTimingResp(Packet *pkt)
{
    auto *ss = safe_cast<StageFunctionWithPort::FuncSenderState *>(
        pkt->senderState);

    ss->stage->recvTimingResp(pkt);
}

// --------- IcachePort ---------

bool
LegoCPU::IcachePort::recvTimingResp(PacketPtr pkt)
{
    DPRINTF(LegoCPU, "IcachePort recvTimingResp\n");
    cpu.recvTimingResp(pkt);
    return true;
}

void
LegoCPU::IcachePort::recvReqRetry()
{
    DPRINTF(LegoCPU, "IcachePort recvReqRetry\n");
    // TODO: retry pending fetch request
}

// --------- DcachePort ---------

bool
LegoCPU::DcachePort::recvTimingResp(PacketPtr pkt)
{
    DPRINTF(LegoCPU, "DcachePort recvTimingResp\n");
    cpu.recvTimingResp(pkt);
    return true;
}

void
LegoCPU::DcachePort::recvReqRetry()
{
    DPRINTF(LegoCPU, "DcachePort recvReqRetry\n");
    // TODO: retry pending data request
}

} // namespace gem5
