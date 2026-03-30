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
    unsigned _readBufferSize, unsigned _priority)
    : ResponsePort(name, _id),
      mem(_mem),
      portId(_id),
      bufferedBlockAddr(~(Addr)0),
      readBufferSize(_readBufferSize),
      priority(_priority),
      retryReq(false),
      retryResp(false)
{}

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
    mem.functionalAccess(pkt);
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
    return mem.recvTimingReq(pkt, portId);
}

void
PipelinedSimpleMemory::MemoryPort::recvRespRetry()
{
    mem.recvRespRetry(portId);
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
      maxOutstanding(p.max_outstanding),
      maxPerPort(p.max_per_port),
      addressPhaseCycles(p.address_phase_cycles),
      nextAcceptTick(0),
      bufferHitCycles(p.buffer_hit_cycles),
      lastResponseTick(0),
      dequeueEvent([this]{ dequeue(); }, name())
{
    for (int i = 0; i < p.port_port_connection_count; ++i) {
        // Per-port read buffer size: use port_read_buffer_size if set,
        // else fall back to read_buffer_size (legacy single-value param).
        unsigned bufSize = getPerPort(p.port_read_buffer_size, i,
                                      p.read_buffer_size);
        unsigned pri = getPerPort(p.port_priority, i, 0);

        fatal_if(bufSize != 0 && !isPowerOf2(bufSize),
                 "port[%d] read_buffer_size must be 0 or power of 2, "
                 "got %d", i, bufSize);

        std::string portName = csprintf("%s.port[%d]", name(), i);
        ports.emplace_back(
            std::make_unique<MemoryPort>(portName, *this, i, bufSize, pri));
        retryEvents.emplace_back(
            [this, i]{ sendRetry(i); }, name(), false,
            Event::Default_Pri + 1);
        portOutstanding.push_back(0);

        DPRINTF(PipelinedMem, "port[%d] created: priority=%d "
                "readBufferSize=%d\n", i, pri, bufSize);
    }
}

Port &
PipelinedSimpleMemory::getPort(const std::string &if_name, PortID idx)
{
    if (if_name == "port") {
        if (idx < ports.size())
            return *ports[idx];
        // idx might be InvalidPortID for legacy single-port usage
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
        // Notify downstream crossbar of our address ranges.
        // Without this, the crossbar's gotAllAddrRanges stays false
        // and it asserts on the first request.
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

// =========================================================================
// Priority helpers
// =========================================================================

bool
PipelinedSimpleMemory::isHighestPriority(PortID portId) const
{
    unsigned myPri = ports[portId]->priority;
    for (size_t i = 0; i < ports.size(); ++i) {
        if ((PortID)i != portId && ports[i]->priority > myPri)
            return false;
    }
    return true;
}

void
PipelinedSimpleMemory::insertSorted(DeferredPacket &&entry)
{
    // Insert sorted by (tick ascending, priority descending).
    // Higher priority responses at the same tick go first.
    auto it = packetQueue.end();
    while (it != packetQueue.begin()) {
        auto prev = std::prev(it);
        if (prev->tick < entry.tick)
            break;
        if (prev->tick == entry.tick && prev->priority >= entry.priority)
            break;
        it = prev;
    }
    packetQueue.insert(it, std::move(entry));
}

// =========================================================================
// recvTimingReq — per-port read buffer, priority, shared address phase
// =========================================================================

bool
PipelinedSimpleMemory::recvTimingReq(PacketPtr pkt, PortID portId)
{
    auto &port = *ports[portId];

    panic_if(pkt->cacheResponding(), "Should not see packets where cache "
             "is responding");

    panic_if(!(pkt->isRead() || pkt->isWrite()),
             "Should only see read and writes at memory controller, "
             "saw %s to %#llx\n", pkt->cmdString(), pkt->getAddr());

    if (port.retryReq)
        return false;

    // Outstanding limit with priority-aware contention.
    bool contention = false;
    for (size_t i = 0; i < ports.size(); ++i)
        if ((PortID)i != portId && portOutstanding[i] != 0)
            contention = true;

    // Highest-priority port can use full maxOutstanding even under
    // contention.  Lower-priority ports are limited to maxPerPort.
    unsigned limit;
    if (!contention || isHighestPriority(portId))
        limit = maxOutstanding;
    else
        limit = maxPerPort;

    if (portOutstanding[portId] >= limit ||
        packetQueue.size() >= maxOutstanding) {
        DPRINTF(PipelinedMem, "port[%d] Rejected %s addr %#x: "
                "portOutstanding=%d/%d total=%d/%d contention=%d "
                "priority=%d\n", portId,
                pkt->cmdString(), pkt->getAddr(),
                portOutstanding[portId], limit,
                (unsigned)packetQueue.size(), maxOutstanding,
                contention, port.priority);
        port.retryReq = true;
        if (!retryEvents[portId].scheduled() && !packetQueue.empty()) {
            DPRINTF(PipelinedMem, "port[%d] Scheduling retry at tick %d\n",
                    portId, packetQueue.front().tick);
            schedule(retryEvents[portId], packetQueue.front().tick);
        }
        return false;
    }

    // Reject if address phase is still occupied (shared)
    if (curTick() < nextAcceptTick) {
        DPRINTF(PipelinedMem, "port[%d] Rejected %s addr %#x: address "
                "phase busy until tick %d\n", portId,
                pkt->cmdString(), pkt->getAddr(), nextAcceptTick);
        port.retryReq = true;
        if (!retryEvents[portId].scheduled()) {
            schedule(retryEvents[portId], nextAcceptTick);
        }
        return false;
    }

    // Consume upstream bus delays
    Tick receive_delay = pkt->headerDelay + pkt->payloadDelay;
    pkt->headerDelay = pkt->payloadDelay = 0;

    // Address phase occupancy (shared)
    nextAcceptTick = clockEdge(addressPhaseCycles);

    // Per-port read buffer lookup (each port has its own buffer size)
    Tick access_latency = getLatency();
    if (pkt->isRead() && port.readBufferSize > 0) {
        Addr blockAddr = pkt->getAddr()
                         & ~(Addr)(port.readBufferSize - 1);
        if (blockAddr == port.bufferedBlockAddr) {
            access_latency = cyclesToTicks(bufferHitCycles);
            DPRINTF(PipelinedMem, "port[%d] Read buffer HIT %s addr %#x "
                    "(block %#x, bufSize=%d): access_latency=%d ticks\n",
                    portId, pkt->cmdString(), pkt->getAddr(),
                    blockAddr, port.readBufferSize, access_latency);
        } else {
            access_latency = getLatency();
            port.bufferedBlockAddr = blockAddr;
            DPRINTF(PipelinedMem, "port[%d] Read buffer MISS %s addr %#x "
                    "(block %#x, bufSize=%d): access_latency=%d ticks, "
                    "buffer filled\n",
                    portId, pkt->cmdString(), pkt->getAddr(),
                    blockAddr, port.readBufferSize, access_latency);
        }
    } else {
        // Write invalidates this port's read buffer if it hits
        if (pkt->isWrite() && port.readBufferSize > 0) {
            Addr blockAddr = pkt->getAddr()
                             & ~(Addr)(port.readBufferSize - 1);
            if (blockAddr == port.bufferedBlockAddr) {
                port.bufferedBlockAddr = ~(Addr)0;
                DPRINTF(PipelinedMem, "port[%d] Write invalidated read "
                        "buffer block %#x\n", portId, blockAddr);
            }
        }
        DPRINTF(PipelinedMem, "port[%d] %s addr %#x: "
                "access_latency=%d ticks (full latency)\n",
                portId, pkt->cmdString(), pkt->getAddr(), access_latency);
    }

    // In-order response delivery (shared data bus)
    Tick data_phase_start = std::max(curTick() + receive_delay,
                                     lastResponseTick);
    Tick when_to_send = data_phase_start + access_latency;

    DPRINTF(PipelinedMem, "port[%d] Accepted %s addr %#x size %d: "
            "portOutstanding=%d total=%d/%d priority=%d "
            "receive_delay=%d access_latency=%d "
            "data_phase_start=%d when_to_send=%d lastResponseTick=%d "
            "nextAcceptTick=%d\n",
            portId, pkt->cmdString(), pkt->getAddr(), pkt->getSize(),
            portOutstanding[portId] + 1,
            (unsigned)packetQueue.size() + 1, maxOutstanding,
            port.priority,
            receive_delay, access_latency, data_phase_start,
            when_to_send, lastResponseTick, nextAcceptTick);

    // Track per-port outstanding count
    ++portOutstanding[portId];

    // Check before access() turns the packet into a response.
    bool needsResponse = pkt->needsResponse();

    // Access backing store (this calls pkt->makeResponse() internally)
    access(pkt);
    lastResponseTick = when_to_send;

    if (needsResponse) {
        assert(pkt->isResponse());

        // Insert sorted by (tick ascending, priority descending)
        insertSorted(DeferredPacket(pkt, when_to_send, portId,
                                    port.priority));

        if (!dequeueEvent.scheduled())
            schedule(dequeueEvent, packetQueue.front().tick);
    } else {
        // No response needed but the write still occupies the data
        // phase.  Decrement portOutstanding immediately (no dequeue
        // will happen for this packet).
        --portOutstanding[portId];
        delete pkt;
    }

    return true;
}

// =========================================================================
// dequeue — send response through the originating port
// =========================================================================

void
PipelinedSimpleMemory::dequeue()
{
    assert(!packetQueue.empty());
    auto &entry = packetQueue.front();
    auto &port = *ports[entry.portId];

    DPRINTF(PipelinedMem, "port[%d] Dequeuing response addr %#x at "
            "tick %d (priority=%d)\n", entry.portId,
            entry.pkt->getAddr(), curTick(), entry.priority);

    port.retryResp = !port.sendTimingResp(entry.pkt);

    if (!port.retryResp) {
        --portOutstanding[entry.portId];
        packetQueue.pop_front();

        if (!packetQueue.empty()) {
            // Use reschedule — event may already be scheduled if
            // packets arrived between the front and now.
            reschedule(dequeueEvent,
                       std::max(packetQueue.front().tick, curTick()),
                       true);
        }
    }
}

// =========================================================================
// Retry — per-port
// =========================================================================

void
PipelinedSimpleMemory::sendRetry(PortID portId)
{
    auto &port = *ports[portId];

    // Priority-aware contention check.
    bool contention = false;
    for (size_t i = 0; i < ports.size(); ++i)
        if ((PortID)i != portId && portOutstanding[i] != 0)
            contention = true;

    unsigned limit;
    if (!contention || isHighestPriority(portId))
        limit = maxOutstanding;
    else
        limit = maxPerPort;

    DPRINTF(PipelinedMem, "port[%d] retryEvent fired: retryReq=%d "
            "portOutstanding=%d/%d total=%d/%d contention=%d "
            "priority=%d\n",
            portId, port.retryReq,
            portOutstanding[portId], limit,
            (unsigned)packetQueue.size(), maxOutstanding,
            contention, port.priority);

    if (port.retryReq &&
        portOutstanding[portId] < limit &&
        packetQueue.size() < maxOutstanding &&
        curTick() >= nextAcceptTick) {
        port.retryReq = false;
        port.sendRetryReq();
    } else if (port.retryReq) {
        // Still can't retry — reschedule for the earliest time the
        // blocking condition could clear.
        Tick retryAt = nextAcceptTick;
        if (!packetQueue.empty())
            retryAt = std::min(retryAt, packetQueue.front().tick);
        retryAt = std::max(retryAt, curTick() + 1);

        if (!retryEvents[portId].scheduled()) {
            DPRINTF(PipelinedMem, "port[%d] Rescheduling retry at %d\n",
                    portId, retryAt);
            schedule(retryEvents[portId], retryAt);
        }
    }
}

void
PipelinedSimpleMemory::recvRespRetry(PortID portId)
{
    auto &port = *ports[portId];
    assert(port.retryResp);
    port.retryResp = false;
    dequeue();
}

} // namespace memory
} // namespace gem5
