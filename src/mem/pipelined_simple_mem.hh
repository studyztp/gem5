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

#ifndef __MEM_PIPELINED_SIMPLE_MEMORY_HH__
#define __MEM_PIPELINED_SIMPLE_MEMORY_HH__

#include <list>
#include <memory>
#include <vector>

#include "base/random.hh"
#include "mem/abstract_mem.hh"
#include "mem/port.hh"
#include "params/PipelinedSimpleMemory.hh"

namespace gem5
{

namespace memory
{

/**
 * Multi-ported pipelined memory with per-port read buffers and priority.
 *
 * Models a flash controller with independent ICode/DCode read ports,
 * per-port read buffers, priority-based scheduling, and AHB-style
 * address/data phase overlap.
 *
 * Per-port state: read buffer (address + size), priority, retry flags.
 * Shared state: flash array (response queue, address phase, outstanding).
 *
 * Priority: higher-priority ports can use up to maxOutstanding even
 * under contention; lower-priority ports are limited to maxPerPort.
 * Response queue is sorted by (tick, priority descending).
 */
class PipelinedSimpleMemory : public AbstractMemory
{
  public:
    /**
     * Inner port class — one instance per connection.
     * Per-port state: read buffer, priority, retry flags.
     */
    class MemoryPort : public ResponsePort
    {
      public:
        PipelinedSimpleMemory &mem;
        const PortID portId;

        /** Per-port read buffer state. */
        Addr bufferedBlockAddr;
        unsigned readBufferSize;  // bytes, 0 = disabled

        /** Per-port priority (higher = higher priority). */
        unsigned priority;

        /** Per-port retry flags. */
        bool retryReq;
        bool retryResp;

        MemoryPort(const std::string &name,
                   PipelinedSimpleMemory &_mem, PortID _id,
                   unsigned _readBufferSize, unsigned _priority);

      protected:
        Tick recvAtomic(PacketPtr pkt) override;
        Tick recvAtomicBackdoor(
            PacketPtr pkt, MemBackdoorPtr &backdoor) override;
        void recvFunctional(PacketPtr pkt) override;
        void recvMemBackdoorReq(
            const MemBackdoorReq &req,
            MemBackdoorPtr &backdoor) override;
        bool recvTimingReq(PacketPtr pkt) override;
        void recvRespRetry() override;
        AddrRangeList getAddrRanges() const override;
    };

  private:
    /** Port vector — one per connection. */
    std::vector<std::unique_ptr<MemoryPort>> ports;

    /** Latency params. */
    const Tick latency;
    const Tick latencyVar;

    /** Maximum total outstanding requests (flash pipeline depth). */
    const unsigned maxOutstanding;

    /** Max outstanding per low-priority port under contention. */
    const unsigned maxPerPort;

    /** Minimum cycles between consecutive request acceptance. */
    const Cycles addressPhaseCycles;

    /** Tick at which the next request can be accepted (shared). */
    Tick nextAcceptTick;

    /** Latency for a read buffer hit. */
    const Cycles bufferHitCycles;

    /** Tick at which the last response will be sent (shared). */
    Tick lastResponseTick;

    /** Pending response with port ID and priority for queue ordering. */
    struct DeferredPacket
    {
        PacketPtr pkt;
        Tick tick;
        PortID portId;
        unsigned priority;

        DeferredPacket(PacketPtr _pkt, Tick _tick, PortID _id, unsigned _pri)
            : pkt(_pkt), tick(_tick), portId(_id), priority(_pri)
        {}
    };

    /** Response queue sorted by (tick, then priority descending). */
    std::list<DeferredPacket> packetQueue;

    /** Event to dequeue completed responses. */
    EventFunctionWrapper dequeueEvent;

    /** Per-port outstanding request count. */
    std::vector<unsigned> portOutstanding;

    /** Per-port retry events. */
    std::vector<EventFunctionWrapper> retryEvents;

    /** Check if this port has the highest priority among all ports. */
    bool isHighestPriority(PortID portId) const;

    /** Insert packet into queue sorted by (tick, priority desc). */
    void insertSorted(DeferredPacket &&entry);

    /** Dequeue and send the front response. */
    void dequeue();

    /** Send retry to upstream on a specific port. */
    void sendRetry(PortID portId);

    /** Random number generator for latency variance. */
    mutable Random::RandomPtr rng = Random::genRandom();

    /** Compute access latency (with optional random variance). */
    Tick getLatency() const;

  public:
    PARAMS(PipelinedSimpleMemory);
    PipelinedSimpleMemory(const Params &p);

    Port &getPort(const std::string &if_name,
                  PortID idx = InvalidPortID) override;

    void init() override;

    /** Handle timing request from a specific port. */
    bool recvTimingReq(PacketPtr pkt, PortID portId);

    /** Handle response retry from a specific port. */
    void recvRespRetry(PortID portId);
};

} // namespace memory
} // namespace gem5

#endif // __MEM_PIPELINED_SIMPLE_MEMORY_HH__
