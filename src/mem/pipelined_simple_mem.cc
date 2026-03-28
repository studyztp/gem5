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

#include "base/intmath.hh"
#include "base/trace.hh"
#include "debug/PipelinedMem.hh"

namespace gem5
{

namespace memory
{

PipelinedSimpleMemory::PipelinedSimpleMemory(
        const PipelinedSimpleMemoryParams &p) :
    SimpleMemory(p),
    maxOutstanding(p.max_outstanding),
    addressPhaseCycles(p.address_phase_cycles),
    nextAcceptTick(0),
    readBufferSize(p.read_buffer_size),
    bufferHitCycles(p.buffer_hit_cycles),
    bufferedBlockAddr(~(Addr)0),
    lastResponseTick(0),
    retryEvent([this]{ sendRetry(); }, name(), false,
               Event::Default_Pri + 1)
{
    fatal_if(readBufferSize != 0 && !isPowerOf2(readBufferSize),
             "read_buffer_size must be 0 or a power of 2, got %d",
             readBufferSize);
}

void
PipelinedSimpleMemory::sendRetry()
{
    DPRINTF(PipelinedMem, "retryEvent fired: retryReq=%d, "
            "packetQueue.size()=%d, maxOutstanding=%d\n",
            retryReq, (unsigned)packetQueue.size(), maxOutstanding);
    if (retryReq && packetQueue.size() < maxOutstanding) {
        retryReq = false;
        sendPortRetryReq();
        DPRINTF(PipelinedMem, "Sent retry, outstanding now %d/%d\n",
                (unsigned)packetQueue.size(), maxOutstanding);
    }
}

bool
PipelinedSimpleMemory::recvTimingReq(PacketPtr pkt)
{
    panic_if(pkt->cacheResponding(), "Should not see packets where cache "
             "is responding");

    panic_if(!(pkt->isRead() || pkt->isWrite()),
             "Should only see read and writes at memory controller, "
             "saw %s to %#llx\n", pkt->cmdString(), pkt->getAddr());

    if (retryReq)
        return false;

    // Reject if at max outstanding
    if (packetQueue.size() >= maxOutstanding) {
        DPRINTF(PipelinedMem, "Rejected %s addr %#x: at max outstanding "
                "(%d/%d)\n", pkt->cmdString(), pkt->getAddr(),
                (unsigned)packetQueue.size(), maxOutstanding);
        retryReq = true;
        // Schedule retry for when the oldest response will be dequeued.
        // retryEvent has lower priority than dequeueEvent, so dequeue
        // runs first at the same tick, popping the entry before
        // retryEvent checks the queue size.
        if (!retryEvent.scheduled() && !packetQueue.empty()) {
            DPRINTF(PipelinedMem, "Scheduling retry at tick %d "
                    "(front dequeue time, pri=%d)\n",
                    packetQueue.front().tick, retryEvent.priority());
            schedule(retryEvent, packetQueue.front().tick);
        }
        return false;
    }

    // Reject if address phase is still occupied
    if (curTick() < nextAcceptTick) {
        DPRINTF(PipelinedMem, "Rejected %s addr %#x: address phase busy "
                "until tick %d (now %d)\n",
                pkt->cmdString(), pkt->getAddr(), nextAcceptTick, curTick());
        retryReq = true;
        // Schedule retry for when the address phase frees
        if (!retryEvent.scheduled()) {
            DPRINTF(PipelinedMem, "Scheduling retry at tick %d "
                    "(address phase free)\n", nextAcceptTick);
            schedule(retryEvent, nextAcceptTick);
        }
        return false;
    }

    // Consume packet delays from upstream bus
    Tick receive_delay = pkt->headerDelay + pkt->payloadDelay;
    DPRINTF(PipelinedMem, "Receiving %s addr %#x size %d: "
            "headerDelay=%d payloadDelay=%d receive_delay=%d\n",
            pkt->cmdString(), pkt->getAddr(), pkt->getSize(),
            pkt->headerDelay, pkt->payloadDelay, receive_delay);
    pkt->headerDelay = pkt->payloadDelay = 0;

    // Address phase occupancy
    nextAcceptTick = clockEdge(addressPhaseCycles);

    // Determine response latency: read buffer hit or full memory access
    Tick access_latency;
    if (pkt->isRead() && readBufferSize > 0) {
        Addr blockAddr = pkt->getAddr() & ~(Addr)(readBufferSize - 1);
        if (blockAddr == bufferedBlockAddr) {
            access_latency = cyclesToTicks(bufferHitCycles);
            DPRINTF(PipelinedMem, "Read buffer HIT %s addr %#x "
                    "(block %#x): access_latency=%d ticks\n",
                    pkt->cmdString(), pkt->getAddr(),
                    blockAddr, access_latency);
        } else {
            access_latency = getLatency();
            bufferedBlockAddr = blockAddr;
            DPRINTF(PipelinedMem, "Read buffer MISS %s addr %#x "
                    "(block %#x): access_latency=%d ticks, buffer filled\n",
                    pkt->cmdString(), pkt->getAddr(),
                    blockAddr, access_latency);
        }
    } else {
        access_latency = getLatency();
        // Writes invalidate the read buffer if they hit the buffered block
        if (pkt->isWrite() && readBufferSize > 0) {
            Addr blockAddr = pkt->getAddr() & ~(Addr)(readBufferSize - 1);
            if (blockAddr == bufferedBlockAddr) {
                bufferedBlockAddr = ~(Addr)0;
                DPRINTF(PipelinedMem, "Write invalidated read buffer "
                        "block %#x\n", blockAddr);
            }
        }
        DPRINTF(PipelinedMem, "%s addr %#x: access_latency=%d ticks "
                "(full latency)\n",
                pkt->cmdString(), pkt->getAddr(), access_latency);
    }

    // Enforce in-order response delivery (AHB ordered bus):
    // This request's data phase can't start until the previous
    // response's data phase completes.
    Tick data_phase_start = std::max(curTick() + receive_delay,
                                     lastResponseTick);
    Tick when_to_send = data_phase_start + access_latency;

    DPRINTF(PipelinedMem, "Accepted %s addr %#x size %d: outstanding %d/%d, "
            "receive_delay=%d access_latency=%d data_phase_start=%d "
            "when_to_send=%d lastResponseTick=%d nextAcceptTick=%d\n",
            pkt->cmdString(), pkt->getAddr(), pkt->getSize(),
            (unsigned)packetQueue.size() + 1, maxOutstanding,
            receive_delay, access_latency, data_phase_start,
            when_to_send, lastResponseTick, nextAcceptTick);

    // Process data (read/write backing store)
    bool needsResponse = pkt->needsResponse();
    recvAtomic(pkt);

    if (needsResponse) {
        assert(pkt->isResponse());

        lastResponseTick = when_to_send;

        // Insert into packetQueue sorted by time, preserving address order
        auto i = packetQueue.end();
        --i;
        while (i != packetQueue.begin() && when_to_send < i->tick &&
               !i->pkt->matchAddr(pkt))
            --i;
        packetQueue.emplace(++i, pkt, when_to_send);

        if (!retryResp && !dequeueEvent.scheduled())
            schedule(dequeueEvent, packetQueue.back().tick);
    } else {
        // Writes also occupy the data phase
        lastResponseTick = when_to_send;
        pendingDelete.reset(pkt);
    }

    return true;
}

} // namespace memory
} // namespace gem5
