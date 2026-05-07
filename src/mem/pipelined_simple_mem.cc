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

#include <iomanip>
#include <sstream>

#include "base/random.hh"
#include "base/trace.hh"
#include "debug/PipelinedMem.hh"
#include "debug/PipelinedMemLineTrace.hh"

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
      portBufferedStreamId(~(uint32_t)0),
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

    // Drop stale entries from the front of readyToFireBuffer.
    // Real silicon's bus matrix cancels speculative AHB transfers
    // for free on a redirect — there is no per-tick retry storm
    // serializing stale-return delivery against new fetches.
    // Modelling the retry would add ~3 cy per redirect on
    // forward-branch kernels (see buffer_hit_sweep / forward
    // line-trace in experiments/raw-data/gem5-stm32g4-verification).
    // F-side filters any remaining in-flight stale responses by
    // streamId at recvTimingResp.
    while (!readyToFireBuffer.empty()
        && readyToFireBuffer.front().pkt->req->hasStreamId()
        && readyToFireBuffer.front().pkt->req->streamId()!=lastStreamId) {
        PacketPtr stale = readyToFireBuffer.front().pkt;
        DPRINTF(PipelinedMem, "port[%d] dropping stale pkt addr=%#x "
                "streamId=%d (current=%d)\n",
                portId, stale->getAddr(),
                stale->req->streamId(), lastStreamId);
        readyToFireBuffer.pop_front();
        delete stale;
    }

    if (!arriveBuffer.empty()
            && (readyToFireBufferSizeLimit==0
            || readyToFireBuffer.size() < readyToFireBufferSizeLimit)) {
        arriveBuffer.front().tick = curTick();
        DPRINTF(PipelinedMem, "port[%d] moving pkt addr=%#x from "
                "arriveBuffer to readyToFire\n",
                portId, arriveBuffer.front().pkt->getAddr());
        Addr movedAddr = arriveBuffer.front().pkt->getAddr();
        readyToFireBuffer.push_back(std::move(arriveBuffer.front()));
        arriveBuffer.pop();
        mem.lineTraceEvent("popArr", portId, movedAddr);
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
    // Drop any stale responses sitting at the head of outputBuffer
    // (Flash fetches that completed for a wrong-path streamId).
    // The bus matrix on real silicon discards these for free; not
    // dropping them here would incur a bus-send + master-reject cycle
    // that delays the next valid response.
    while (!outputBuffer.empty()
            && outputBuffer.front().pkt->req->hasStreamId()
            && outputBuffer.front().pkt->req->streamId()!=lastStreamId) {
        PacketPtr stale = outputBuffer.front().pkt;
        DPRINTF(PipelinedMem, "port[%d] dropping stale output addr=%#x "
                "streamId=%d (current=%d)\n",
                portId, stale->getAddr(),
                stale->req->streamId(), lastStreamId);
        outputBuffer.pop_front();
        delete stale;
    }
    if (!outputBuffer.empty()) {
        DPRINTF(PipelinedMem, "port[%d] popOutputBuffer: sending resp "
                "addr=%#x type=%d tick=%d\n",
                portId, outputBuffer.front().pkt->getAddr(),
                outputBuffer.front().returnType, outputBuffer.front().tick);
        mem.lineTraceEvent("respDeliver", portId,
                           outputBuffer.front().pkt->getAddr(),
                           outputBuffer.front().returnType);
        retryResp = !sendTimingResp(outputBuffer.front().pkt);
        if (!retryResp) {
            if (outputBuffer.front().returnType == ReturnFromMem
                    && portBufferBlockSize > 0) {
                PacketPtr filledPkt = outputBuffer.front().pkt;
                portBufferedBlockAddr = filledPkt->getAddr()
                                            & ~(Addr)(portBufferBlockSize - 1);
                // Tag with streamId only if the request carries one;
                // untagged requests fall back to address-only matching
                // on the consumer side (see popReadyToFireBuffer).
                if (filledPkt->req->hasStreamId()) {
                    portBufferedStreamId = filledPkt->req->streamId();
                } else {
                    portBufferedStreamId = ~(uint32_t)0;
                }
                DPRINTF(PipelinedMem, "port[%d] buffer updated: "
                        "blockAddr=%#x bufferedStreamId=%u\n",
                        portId, portBufferedBlockAddr,
                        portBufferedStreamId);
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
        PacketPtr pkt = readyToFireBuffer.front().pkt;
        Addr pktAddr = pkt->getAddr();
        // ICode requests (Fetch) carry a streamId so the buffer can
        // model the silicon Flash sense-amp being overwritten on a
        // taken-branch redirect (streamId bumps in Fetch::applyRedirect).
        // DCode loads from the LSQ and any system-port functional
        // accesses don't set one — for those we fall back to
        // address-only matching, which preserves their existing
        // buffer-hit behaviour.
        const bool pktHasStreamId = pkt->req->hasStreamId();
        DPRINTF(PipelinedMem, "port[%d] popReadyToFire: addr=%#x "
                "hasStreamId=%d bufferBlock=%#x bufferStreamId=%u\n",
                portId, pktAddr, (int)pktHasStreamId,
                portBufferedBlockAddr, portBufferedStreamId);

        if (portBufferBlockSize > 0) {
            Addr pktBlockAddr = pktAddr & ~(Addr)(portBufferBlockSize - 1);
            const bool streamIdOk =
                !pktHasStreamId
                || pkt->req->streamId() == portBufferedStreamId;
            if (pktBlockAddr == portBufferedBlockAddr && streamIdOk) {
                DPRINTF(PipelinedMem, "port[%d] BUFFER HIT addr=%#x "
                        "block=%#x\n", portId, pktAddr, pktBlockAddr);
                mem.lineTraceEvent("hit", portId, pktAddr,
                                   ReturnFromBuffer);
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
        mem.lineTraceEvent("miss", portId, pktAddr);
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
                mem.lineTraceEvent("piggyback", portId,
                    readyToFireBuffer.front().pkt->getAddr(),
                    ReturnFromPiggyback);
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
                    "dropping stale arrivals\n",
                    portId, lastStreamId, streamId);
            lastStreamId = streamId;
            // Drop stale arrivals immediately — see comment in
            // popArriveBuffer for the structural rationale (real
            // bus matrix cancels speculative transfers for free).
            while (!arriveBuffer.empty()
                    && arriveBuffer.front().pkt->req->hasStreamId()
                    && arriveBuffer.front().pkt->req->streamId()!=streamId) {
                PacketPtr stale = arriveBuffer.front().pkt;
                DPRINTF(PipelinedMem, "port[%d] dropping stale arrival "
                        "addr=%#x streamId=%d (current=%d)\n",
                        portId, stale->getAddr(),
                        stale->req->streamId(), streamId);
                mem.lineTraceEvent("retract", portId, stale->getAddr());
                arriveBuffer.pop();
                delete stale;
            }
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
    mem.lineTraceEvent("req", portId, pkt->getAddr());
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
    lineTraceEvent("fetchStart", currentFetchingPkt->portId,
                   currentFetchingPkt->pkt->getAddr(), ReturnFromMem);
    return nextAcceptTick;
}

void
PipelinedSimpleMemory::retry() {
    recvTimingReq();
}

// ===========================================================================
// PipelinedMemLineTrace — table-format trace aligned to CPU cycle boundaries
// ===========================================================================
//
// Each row of the table is one event from the memory's perspective.  Columns:
//   tick    — absolute simulation tick (correlate with the CPU's
//             Signal3CPULineTrace via tick).
//   cycle   — tick / 5882 (CPU cycle at 170 MHz; helpful when reading by eye)
//   event   — short tag (req, popArr, fire, hit, miss, fetchStart,
//             respDeliver, retract, retry, ...)
//   port    — port index (0=icache, 1=dcache typically)
//   addr    — packet address
//   dTick   — relative tick remaining for the in-flight Flash transfer
//             (nextAcceptTick - curTick) when a request is "fired", or "-"
//   arr/rtf/out — per-port queue depths after the event
//   curBlk  — currentFetchingBlockAddr (Flash sense-amp's in-progress line)
//   bufBlk  — port's portBufferedBlockAddr (the latched 64-bit line)
//
// All columns are fixed-width so the table aligns when read with `awk`.

namespace
{
constexpr unsigned kTickCol  = 12;
constexpr unsigned kCycCol   = 7;
constexpr unsigned kEvCol    = 12;
constexpr unsigned kPortCol  = 4;
constexpr unsigned kAddrCol  = 12;
constexpr unsigned kDTickCol = 8;
constexpr unsigned kQCol     = 13;   // arr/rtf/out triple
constexpr unsigned kBlkCol   = 12;

constexpr Tick kTickPerCycle170MHz = 5882;

std::string padLeft(const std::string &s, unsigned w)
{
    if (s.size() >= w) return s;
    return std::string(w - s.size(), ' ') + s;
}
std::string padRight(const std::string &s, unsigned w)
{
    if (s.size() >= w) return s.substr(0, w);
    return s + std::string(w - s.size(), ' ');
}

std::string sepBar()
{
    std::string b = "+-" + std::string(kTickCol, '-')
                  + "-+-" + std::string(kCycCol, '-')
                  + "-+-" + std::string(kEvCol, '-')
                  + "-+-" + std::string(kPortCol, '-')
                  + "-+-" + std::string(kAddrCol, '-')
                  + "-+-" + std::string(kDTickCol, '-')
                  + "-+-" + std::string(kQCol, '-')
                  + "-+-" + std::string(kBlkCol, '-')
                  + "-+-" + std::string(kBlkCol, '-')
                  + "-+";
    return b;
}
}  // namespace

void
PipelinedSimpleMemory::lineTraceEvent(const std::string &event, PortID port,
                                      Addr addr, ReturnType rt)
{
    if (!debug::PipelinedMemLineTrace) return;

    if (!_lineTraceHeaderEmitted) {
        std::string bar = sepBar();
        DPRINTF(PipelinedMemLineTrace, "%s\n", bar.c_str());
        DPRINTF(PipelinedMemLineTrace,
            "| %s | %s | %s | %s | %s | %s | %s | %s | %s |\n",
            padLeft("tick", kTickCol).c_str(),
            padLeft("cycle", kCycCol).c_str(),
            padRight("event", kEvCol).c_str(),
            padRight("port", kPortCol).c_str(),
            padRight("addr", kAddrCol).c_str(),
            padRight("dTick", kDTickCol).c_str(),
            padRight("arr/rtf/out", kQCol).c_str(),
            padRight("curBlk", kBlkCol).c_str(),
            padRight("bufBlk", kBlkCol).c_str());
        DPRINTF(PipelinedMemLineTrace, "%s\n", bar.c_str());
        _lineTraceHeaderEmitted = true;
    }

    const Tick now = curTick();
    const auto cyc = now / kTickPerCycle170MHz;

    std::string dTickS = "-";
    if (nextAcceptTick > now) {
        std::ostringstream os;
        os << (nextAcceptTick - now);
        dTickS = os.str();
    }

    // Per-port queue depths.
    std::string qS;
    {
        std::ostringstream os;
        if (port >= 0 && (size_t)port < ports.size()) {
            const auto &p = *ports[port];
            os << p.arriveBuffer.size() << "/"
               << p.readyToFireBuffer.size() << "/"
               << p.outputBuffer.size();
        } else {
            os << "-";
        }
        qS = os.str();
    }

    std::ostringstream addrS;
    addrS << "0x" << std::hex << addr;

    std::ostringstream curS;
    curS << "0x" << std::hex << currentFetchingBlockAddr;

    std::ostringstream bufS;
    if (port >= 0 && (size_t)port < ports.size()) {
        bufS << "0x" << std::hex << ports[port]->portBufferedBlockAddr;
    } else {
        bufS << "-";
    }

    std::string evTag = event;
    if (rt == ReturnFromMem)         evTag += "<mem";
    else if (rt == ReturnFromBuffer) evTag += "<buf";
    else if (rt == ReturnFromPiggyback) evTag += "<pig";

    DPRINTF(PipelinedMemLineTrace,
        "| %s | %s | %s | %s | %s | %s | %s | %s | %s |\n",
        padLeft(std::to_string(now), kTickCol).c_str(),
        padLeft(std::to_string(cyc), kCycCol).c_str(),
        padRight(evTag, kEvCol).c_str(),
        padLeft(std::to_string((int)port), kPortCol).c_str(),
        padRight(addrS.str(), kAddrCol).c_str(),
        padRight(dTickS, kDTickCol).c_str(),
        padRight(qS, kQCol).c_str(),
        padRight(curS.str(), kBlkCol).c_str(),
        padRight(bufS.str(), kBlkCol).c_str());
}

} // namespace memory
} // namespace gem5
