/*
 * Copyright (c) 2026 University of California, Davis and Cornell University
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

#include "mem/pipelined_simple_mem.hh"

#include "base/random.hh"
#include "base/trace.hh"
#include "debug/PipelinedMem.hh"

namespace gem5
{

namespace memory
{

// =========================================================================
// MemoryPort — inner port class
// =========================================================================

PipelinedSimpleMemory::MemoryPort::MemoryPort(
    const std::string &name, PipelinedSimpleMemory &_mem, PortID _id,
    unsigned _readBufferSize, unsigned _priority,
    size_t _arriveLimit, size_t _readyToFireLimit)
    : ResponsePort(name, _id),
      mem(_mem),
      portId(_id),
      portBufferedBlockAddr(~(Addr)0),
      portBufferBlockSize(_readBufferSize),
      lastStreamId(0),
      priority(_priority),
      retryReq(false),
      retryResp(false),
      arriveBufferSizeLimit(_arriveLimit),
      popArriveBufferEvent([this]{popArriveBuffer();}, name+"_popArriveBuffer",
          false, Event::Default_Pri),
      popOutputBufferEvent([this]{popOutputBuffer();}, name+"_popOutputBuffer",
          false, Event::Default_Pri - 1),
      readyToFireBufferSizeLimit(_readyToFireLimit),
      staleReturnEvent([this]{retryStaleReturn();}, name+"_staleReturn"),
      addressPhaseLatency(_mem.params().address_phase_latency),
      bufferHitLatency(_mem.params().buffer_hit_latency)
{}

void
PipelinedSimpleMemory::MemoryPort::popArriveBuffer() {
    DPRINTF(PipelinedMem, "port[%d] popArriveBuffer: arriveBuffer=%d "
            "readyToFire=%d outputBuffer=%d\n",
            portId, arriveBuffer.size(), readyToFireBuffer.size(),
            outputBuffer.size());

    while (!readyToFireBuffer.empty()
        && readyToFireBuffer.front().pkt->req->hasStreamId()
        && readyToFireBuffer.front().pkt->req->streamId()!=lastStreamId) {
        PacketPtr stale = readyToFireBuffer.front().pkt;
        DPRINTF(PipelinedMem, "port[%d] returning stale pkt addr=%#x "
                "streamId=%d (current=%d)\n",
                portId, stale->getAddr(),
                stale->req->streamId(), lastStreamId);
        stale->makeResponse();
        stale->setBadAddress();
        readyToFireBuffer.pop_front();
        if (retryResp) {
            staleReturnQueue.push_back(stale);
        } else if (!sendTimingResp(stale)) {
            retryResp = true;
            staleReturnQueue.push_back(stale);
        }
    }

    if (!arriveBuffer.empty()
            && (readyToFireBufferSizeLimit==0
            || readyToFireBuffer.size() < readyToFireBufferSizeLimit)) {
        arriveBuffer.front().tick = curTick();
        DPRINTF(PipelinedMem, "port[%d] moving pkt addr=%#x from "
                "arriveBuffer to readyToFire\n",
                portId, arriveBuffer.front().pkt->getAddr());
        readyToFireBuffer.push_back(std::move(arriveBuffer.front()));
        arriveBuffer.pop();
    }

    popReadyToFireBuffer();

    /* If arriveBuffer still has entries, schedule another address phase
     * so the next packet can reach readyToFireBuffer while flash is busy
     * (enabling piggyback). */
    if (!arriveBuffer.empty() && !popArriveBufferEvent.scheduled()) {
        Tick nextAddr = curTick() + addressPhaseLatency;
        DPRINTF(PipelinedMem, "port[%d] arriveBuffer not empty, "
                "scheduling next address phase at tick=%d\n",
                portId, nextAddr);
        mem.schedule(popArriveBufferEvent, std::max(nextAddr, curTick()));
    }
}

void
PipelinedSimpleMemory::MemoryPort::popOutputBuffer() {
    if (retryResp)
        return;
    if (!outputBuffer.empty()) {
        DPRINTF(PipelinedMem, "port[%d] popOutputBuffer: sending resp "
                "addr=%#x type=%d tick=%d\n",
                portId, outputBuffer.front().pkt->getAddr(),
                outputBuffer.front().returnType, outputBuffer.front().tick);
        retryResp = !sendTimingResp(outputBuffer.front().pkt);
        if (!retryResp) {
            if (outputBuffer.front().returnType == ReturnFromMem
                    && portBufferBlockSize > 0) {
                portBufferedBlockAddr = outputBuffer.front().pkt->getAddr()
                                            & ~(Addr)(portBufferBlockSize - 1);
                DPRINTF(PipelinedMem, "port[%d] buffer updated: "
                        "blockAddr=%#x\n", portId, portBufferedBlockAddr);
            }
            outputBuffer.pop_front();
            if (!outputBuffer.empty()) {
                DPRINTF(PipelinedMem, "port[%d] next output at tick=%d\n",
                        portId, outputBuffer.front().tick);
                mem.reschedule(popOutputBufferEvent,
                    std::max(outputBuffer.front().tick, curTick()),
                    true);
            }
        } else {
            DPRINTF(PipelinedMem, "port[%d] sendTimingResp failed, "
                    "waiting for retry\n", portId);
        }
    }
}

void
PipelinedSimpleMemory::MemoryPort::popReadyToFireBuffer() {
    if (!readyToFireBuffer.empty()) {
        Addr pktAddr = readyToFireBuffer.front().pkt->getAddr();
        DPRINTF(PipelinedMem, "port[%d] popReadyToFire: addr=%#x "
                "bufferBlock=%#x\n", portId, pktAddr, portBufferedBlockAddr);

        if (portBufferBlockSize > 0) {
            Addr pktBlockAddr = pktAddr & ~(Addr)(portBufferBlockSize - 1);
            if (pktBlockAddr == portBufferedBlockAddr) {
                DPRINTF(PipelinedMem, "port[%d] BUFFER HIT addr=%#x "
                        "block=%#x\n", portId, pktAddr, pktBlockAddr);
                readyToFireBuffer.front().tick = curTick() + bufferHitLatency;
                readyToFireBuffer.front().returnType = ReturnFromBuffer;
                outputBuffer.push_back(readyToFireBuffer.front());
                mem.access(readyToFireBuffer.front().pkt);
                readyToFireBuffer.pop_front();
                scheduleNextPopArriveBuffer(curTick() + bufferHitLatency);
                return;
            }
        }

        DPRINTF(PipelinedMem, "port[%d] BUFFER MISS addr=%#x, "
                "requesting flash\n", portId, pktAddr);
        Tick nextAcceptTick = mem.recvTimingReq();
        if (!readyToFireBuffer.empty() && mem.currentFetchingPkt
                && portId == mem.currentFetchingPkt->portId) {
            Addr pktMemReadAddr =
                readyToFireBuffer.front().pkt->getAddr()
                & ~(Addr)(mem.readMemoryRequestSize - 1);
            if (pktMemReadAddr == mem.currentFetchingBlockAddr) {
                DPRINTF(PipelinedMem, "port[%d] PIGGYBACK addr=%#x on "
                        "fetchBlock=%#x, ready at tick=%d\n",
                        portId, readyToFireBuffer.front().pkt->getAddr(),
                        mem.currentFetchingBlockAddr, nextAcceptTick);
                readyToFireBuffer.front().tick = nextAcceptTick;
                readyToFireBuffer.front().returnType = ReturnFromPiggyback;
                outputBuffer.push_back(readyToFireBuffer.front());
                mem.access(readyToFireBuffer.front().pkt);
                readyToFireBuffer.pop_front();
            }
        }
        scheduleNextPopArriveBuffer(nextAcceptTick);
    }
}

void
PipelinedSimpleMemory::MemoryPort::scheduleNextPopArriveBuffer(Tick nextScheduleTime) {
    if (!outputBuffer.empty() && !popOutputBufferEvent.scheduled()) {
        Tick outTick = std::max(outputBuffer.front().tick, curTick());
        DPRINTF(PipelinedMem, "port[%d] scheduling popOutputBuffer at "
                "tick=%d\n", portId, outTick);
        mem.schedule(popOutputBufferEvent, outTick);
    }
    {
        Tick schedTick;
        if (!outputBuffer.empty() && !readyToFireBuffer.empty()) {
            /* readyToFireBuffer has a pending request waiting for flash.
             * Schedule at flash-done time so we don't retry before flash
             * is ready (which would cause an infinite loop). */
            schedTick = std::max(nextScheduleTime, curTick());
        } else if (!outputBuffer.empty()) {
            /* outputBuffer has responses to send, no pending flash request.
             * Schedule when the response is ready. */
            schedTick = std::max(outputBuffer.front().tick, curTick());
        } else {
            schedTick = std::max(nextScheduleTime, curTick());
        }
        if (!popArriveBufferEvent.scheduled()) {
            DPRINTF(PipelinedMem, "port[%d] scheduling popArriveBuffer at "
                    "tick=%d\n", portId, schedTick);
            mem.schedule(popArriveBufferEvent, schedTick);
        }
    }

}

void
PipelinedSimpleMemory::MemoryPort::retryStaleReturn() {
    if (retryResp)
        return;
    while (!staleReturnQueue.empty()) {
        PacketPtr stale = staleReturnQueue.front();
        DPRINTF(PipelinedMem, "port[%d] retrying stale return addr=%#x\n",
                portId, stale->getAddr());
        retryResp = !sendTimingResp(stale);
        if (!retryResp) {
            staleReturnQueue.pop_front();
        } else {
            DPRINTF(PipelinedMem, "port[%d] stale return failed, "
                    "waiting for recvRespRetry\n", portId);
            return;
        }
    }
}

Tick
PipelinedSimpleMemory::MemoryPort::recvAtomic(PacketPtr pkt)
{
    mem.access(pkt);
    return mem.getLatency();
}

Tick
PipelinedSimpleMemory::MemoryPort::recvAtomicBackdoor(
    PacketPtr pkt, MemBackdoorPtr &backdoor)
{
    Tick latency = recvAtomic(pkt);
    mem.getBackdoor(backdoor);
    return latency;
}

void
PipelinedSimpleMemory::MemoryPort::recvFunctional(PacketPtr pkt)
{
    pkt->pushLabel(mem.name());

    mem.functionalAccess(pkt);

    // Check in-flight packets across all per-port buffers
    for (auto &port : mem.ports) {
        for (auto &dp : port->outputBuffer) {
            if (pkt->trySatisfyFunctional(dp.pkt))
                break;
        }
        for (auto &dp : port->readyToFireBuffer) {
            if (pkt->trySatisfyFunctional(dp.pkt))
                break;
        }
    }

    pkt->popLabel();
}

void
PipelinedSimpleMemory::MemoryPort::recvMemBackdoorReq(
    const MemBackdoorReq &req, MemBackdoorPtr &backdoor)
{
    mem.getBackdoor(backdoor);
}

bool
PipelinedSimpleMemory::MemoryPort::recvTimingReq(PacketPtr pkt)
{
    DPRINTF(PipelinedMem, "port[%d] recvTimingReq: addr=%#x size=%d %s\n",
            portId, pkt->getAddr(), pkt->getSize(), pkt->cmdString());

    if (pkt->req->hasStreamId()) {
        uint32_t streamId = pkt->req->streamId();
        if (lastStreamId != streamId) {
            DPRINTF(PipelinedMem, "port[%d] stream change %d -> %d, "
                    "flushing stale arrivals\n",
                    portId, lastStreamId, streamId);
            lastStreamId = streamId;
            while (!arriveBuffer.empty()
                    && arriveBuffer.front().pkt->req->streamId()!=streamId) {
                PacketPtr stale = arriveBuffer.front().pkt;
                DPRINTF(PipelinedMem, "port[%d] returning stale arrival "
                        "addr=%#x streamId=%d (current=%d)\n",
                        portId, stale->getAddr(),
                        stale->req->streamId(), streamId);
                stale->makeResponse();
                stale->setBadAddress();
                arriveBuffer.pop();
                staleReturnQueue.push_back(stale);
            }
            if (!staleReturnQueue.empty() && !retryResp
                    && !staleReturnEvent.scheduled())
                mem.schedule(staleReturnEvent, curTick() + 1);
        }
    }
    if (arriveBufferSizeLimit != 0
            && arriveBuffer.size() >= arriveBufferSizeLimit) {
        DPRINTF(PipelinedMem, "port[%d] arriveBuffer full (%d/%d), "
                "rejecting\n", portId, arriveBuffer.size(),
                arriveBufferSizeLimit);
        retryReq = true;
        return false;
    }
    arriveBuffer.push(DeferredPacket(pkt, curTick(), portId, priority, NotScheduled));
    Tick addrPhaseDone = curTick() + addressPhaseLatency;
    DPRINTF(PipelinedMem, "port[%d] accepted, arriveBuffer=%d, "
            "scheduling popArriveBuffer at tick=%d\n",
            portId, arriveBuffer.size(), addrPhaseDone);
    if (!popArriveBufferEvent.scheduled()) {
        mem.schedule(popArriveBufferEvent, addrPhaseDone);
    }
    return true;
}

void
PipelinedSimpleMemory::MemoryPort::recvRespRetry()
{
    assert(retryResp);
    retryResp = false;
    popOutputBuffer();
    retryStaleReturn();
}

AddrRangeList
PipelinedSimpleMemory::MemoryPort::getAddrRanges() const
{
    AddrRangeList ranges;
    ranges.push_back(mem.getAddrRange());
    return ranges;
}

// =========================================================================
// PipelinedSimpleMemory — constructor and port management
// =========================================================================

/** Helper: get value from a vector param, reusing last entry if short. */
static unsigned
getPerPort(const std::vector<unsigned> &vec, unsigned idx, unsigned fallback)
{
    if (vec.empty())
        return fallback;
    return (idx < vec.size()) ? vec[idx] : vec.back();
}

PipelinedSimpleMemory::PipelinedSimpleMemory(const Params &p)
    : AbstractMemory(p),
      latency(p.latency),
      latencyVar(p.latency_var),
      nextAcceptTick(0),
      currentFetchingBlockAddr(~(Addr)0),
      readMemoryRequestSize(p.memory_read_request_size),
      retryEvent([this]{ retry(); }, name())
{
    for (int i = 0; i < p.port_port_connection_count; ++i) {
        unsigned bufSize = getPerPort(p.port_read_buffer_size, i,
                                      p.read_buffer_size);
        unsigned pri = getPerPort(p.port_priority, i, 0);
        unsigned arriveLimit = getPerPort(p.port_arrive_buffer_size, i,
                                          p.arrive_buffer_size);
        unsigned readyToFireLimit = getPerPort(
                p.port_ready_to_fire_buffer_size, i,
                p.ready_to_fire_buffer_size);

        fatal_if(bufSize != 0 && !isPowerOf2(bufSize),
                 "port[%d] read_buffer_size must be 0 or power of 2, "
                 "got %d", i, bufSize);
        fatal_if(bufSize > p.memory_read_request_size,
                 "port[%d] read_buffer_size (%d) exceeds "
                 "memory_read_request_size (%d)",
                 i, bufSize, p.memory_read_request_size);

        std::string portName = csprintf("%s.port[%d]", name(), i);
        ports.emplace_back(
            std::make_unique<MemoryPort>(portName, *this, i, bufSize, pri,
                                         arriveLimit, readyToFireLimit));

        DPRINTF(PipelinedMem, "port[%d] created: priority=%d "
                "readBufferSize=%d arriveLimit=%d readyToFireLimit=%d\n",
                i, pri, bufSize, arriveLimit, readyToFireLimit);
    }
}

Port &
PipelinedSimpleMemory::getPort(const std::string &if_name, PortID idx)
{
    if (if_name == "port") {
        if (idx < ports.size())
            return *ports[idx];
        if (idx == InvalidPortID && ports.size() == 1)
            return *ports[0];
    }
    return AbstractMemory::getPort(if_name, idx);
}

void
PipelinedSimpleMemory::init()
{
    AbstractMemory::init();
    for (auto &p : ports) {
        if (!p->isConnected())
            fatal("%s: port %s is not connected.\n", name(), p->name());
        p->sendRangeChange();
    }
}

Tick
PipelinedSimpleMemory::getLatency() const
{
    if (latencyVar)
        return latency + rng->random<Tick>(0, latencyVar * 2);
    return latency;
}

DrainState
PipelinedSimpleMemory::drain()
{
    for (auto &port : ports) {
        if (!port->outputBuffer.empty() || !port->readyToFireBuffer.empty()
                || !port->arriveBuffer.empty()) {
            return DrainState::Draining;
        }
    }
    return DrainState::Drained;
}

Tick
PipelinedSimpleMemory::recvTimingReq()
{
    if (curTick() < nextAcceptTick) {
        DPRINTF(PipelinedMem, "flash busy until tick=%d\n", nextAcceptTick);
        return nextAcceptTick;
    }
    currentFetchingBlockAddr = 0;

    DeferredPacket *best = nullptr;
    for (size_t i = 0; i < ports.size(); ++i) {
        if (!ports[i]->readyToFireBuffer.empty()) {
            auto &candidate = ports[i]->readyToFireBuffer.front();
            if (!best
                || candidate.tick < best->tick
                || (candidate.tick == best->tick
                    && candidate.priority > best->priority)) {
                best = &candidate;
            }
        }
    }

    if (!best) {
        DPRINTF(PipelinedMem, "no ready packets across any port\n");
        return nextAcceptTick;
    }

    nextAcceptTick = curTick() + getLatency();

    currentFetchingPkt = *best;
    currentFetchingBlockAddr = currentFetchingPkt->pkt->getAddr()
                                        & ~(Addr)(readMemoryRequestSize - 1);

    auto &targetPort = ports[currentFetchingPkt->portId];
    targetPort->readyToFireBuffer.pop_front();
    currentFetchingPkt->returnType = ReturnFromMem;
    currentFetchingPkt->tick = nextAcceptTick;
    targetPort->outputBuffer.push_back(*currentFetchingPkt);
    access(currentFetchingPkt->pkt);

    if (!targetPort->popOutputBufferEvent.scheduled()) {
        schedule(targetPort->popOutputBufferEvent, nextAcceptTick);
    }
    if (!targetPort->popArriveBufferEvent.scheduled()) {
        schedule(targetPort->popArriveBufferEvent, nextAcceptTick);
    }

    DPRINTF(PipelinedMem, "flash FETCH: port[%d] addr=%#x block=%#x "
            "latency=%d ready at tick=%d\n",
            currentFetchingPkt->portId,
            currentFetchingPkt->pkt->getAddr(),
            currentFetchingBlockAddr,
            nextAcceptTick - curTick(), nextAcceptTick);
    return nextAcceptTick;
}

void
PipelinedSimpleMemory::retry() {
    recvTimingReq();
}


} // namespace memory
} // namespace gem5
