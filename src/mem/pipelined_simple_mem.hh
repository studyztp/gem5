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
#include <optional>
#include <queue>
#include <vector>

#include "base/random.hh"
#include "mem/abstract_mem.hh"
#include "mem/port.hh"
#include "params/PipelinedSimpleMemory.hh"

namespace gem5
{

namespace memory
{

class PipelinedSimpleMemory : public AbstractMemory
{
  public:
    enum ReturnType
    {
      ReturnFromMem,
      ReturnFromBuffer,
      ReturnFromPiggyback,
      NotScheduled
    };

    struct DeferredPacket
    {
        PacketPtr pkt;
        Tick tick;
        PortID portId;
        unsigned priority;
        ReturnType returnType;

        DeferredPacket(PacketPtr _pkt, Tick _tick, PortID _id,
            unsigned _p, ReturnType _rt)
            : pkt(_pkt), tick(_tick), portId(_id),
              priority(_p), returnType(_rt)
        {}
    };

    /**
     * Inner port class — one instance per connection.
     * Per-port state: read buffer, pending read, priority, retry flags.
     */
    class MemoryPort : public ResponsePort
    {
      public:
        PipelinedSimpleMemory &mem;
        const PortID portId;

        /** Per-port current buffer state (filled at dequeue time).
         *  The buffer models the Flash sense-amp output latch
         *  (RM0440 §4.3.4), which holds the most-recently-completed
         *  read.  `portBufferedStreamId` records which CPU stream
         *  filled it; a hit requires both block-address and
         *  streamId to match — modelling that the sense-amp is
         *  overwritten by every Flash read, including wrong-path
         *  prefetches that completed before a redirect. */
        Addr portBufferedBlockAddr;
        unsigned portBufferBlockSize;  // bytes, 0 = disabled
        uint32_t portBufferedStreamId;

        /**
         * Per-port stream tracking for AHB transfer retraction.
         * When a new request arrives with a different streamId, the
         * port resets flash availability — modeling the CM4's ability
         * to retract pending AHB transfers [DDI0439D §2.2.1].
         */
        uint32_t lastStreamId;

        /** Per-port priority (higher = higher priority). */
        unsigned priority;

        /** Per-port retry flags. */
        bool retryReq;
        bool retryResp;

        // buffers
        std::queue<DeferredPacket> arriveBuffer;
        size_t arriveBufferSizeLimit;
        void popArriveBuffer();
        EventFunctionWrapper popArriveBufferEvent;

        std::list<DeferredPacket> outputBuffer;
        void popOutputBuffer();
        EventFunctionWrapper popOutputBufferEvent;
        std::list<DeferredPacket> readyToFireBuffer;
        size_t readyToFireBufferSizeLimit;
        void popReadyToFireBuffer();
        void scheduleNextPopArriveBuffer(Tick nextScheduleTime);

        std::list<PacketPtr> staleReturnQueue;
        void retryStaleReturn();
        EventFunctionWrapper staleReturnEvent;

        const Tick addressPhaseLatency;
        const Tick bufferHitLatency;

        MemoryPort(const std::string &name,
                   PipelinedSimpleMemory &_mem, PortID _id,
                   unsigned _readBufferSize, unsigned _priority,
                   size_t _arriveLimit, size_t _readyToFireLimit);

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

    /** Tick at which the next request can be accepted (shared). */
    Tick nextAcceptTick;

    std::optional<DeferredPacket> currentFetchingPkt;
    Addr currentFetchingBlockAddr;
    size_t readMemoryRequestSize;

    void retry();
    EventFunctionWrapper retryEvent;

    mutable Random::RandomPtr rng = Random::genRandom();

    Tick getLatency() const;

  public:
    PARAMS(PipelinedSimpleMemory);
    PipelinedSimpleMemory(const Params &p);

    Port &getPort(const std::string &if_name,
                  PortID idx = InvalidPortID) override;

    DrainState drain() override;
    void init() override;

    Tick recvTimingReq();

    /** Emit a structured table row under PipelinedMemLineTrace.
     *
     *  Each row: tick | event | port | addr | nextAccept (relative) |
     *            arr/rtf/out counts | currentFetchingBlock | bufferedBlock
     *  Designed so a 5882-tick CPU cycle boundary aligns with the
     *  CPU's Signal3CPULineTrace rows for tick-by-tick correlation.
     *  No-op when the flag is off (cheap to leave instrumented). */
    void lineTraceEvent(const std::string &event, PortID port,
                        Addr addr, ReturnType rt = NotScheduled);

  private:
    /** True iff the trace header has been emitted (one-shot). */
    bool _lineTraceHeaderEmitted = false;
};

} // namespace memory
} // namespace gem5

#endif // __MEM_PIPELINED_SIMPLE_MEMORY_HH__
