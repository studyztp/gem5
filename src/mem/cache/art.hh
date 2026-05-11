/**
 * @file
 * Declares the ART (Adaptive Real-Time) accelerator cache model.
 *
 * The ART accelerator is modelled after the STM32 ART instruction
 * accelerator, which sits between the CPU and flash memory.  It uses a
 * two-buffer sequential prefetch scheme:
 *
 *  - prefetchBuffer:  holds the line currently being (or just) fetched
 *                     from memory.
 *  - currentBuffer:   holds the previously prefetched line that is
 *                     available for immediate CPU hits.
 *
 * An optional direct-memory mode bypasses the cache entirely and
 * forwards every request straight to memory, modelling the STM32
 * zero-wait-state flash access path.
 *
 * @note The prefetch logic is implemented directly in this cache class
 *       rather than through gem5's probe-based prefetcher framework,
 *       because the two-buffer model does not map naturally onto the
 *       MSHR-based prefetch flow.  A future refactor could encapsulate
 *       the sequential-prefetch logic in a prefetch::Base subclass.
 */

#ifndef __MEM_CACHE_ART_HH__
#define __MEM_CACHE_ART_HH__

#include <deque>
#include <list>
#include <memory>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

#include "base/intmath.hh"
#include "base/printable.hh"
#include "base/statistics.hh"
#include "base/trace.hh"
#include "base/types.hh"
#include "debug/ARTBuffers.hh"
#include "debug/ARTBypass.hh"
#include "debug/ARTCache.hh"
#include "debug/ARTCacheLineTrace.hh"
#include "debug/ARTPipeline.hh"
#include "debug/ARTPrefetch.hh"
#include "debug/Cache.hh"
#include "mem/cache/noncoherent_cache.hh"
#include "mem/cache/queue_entry.hh"
#include "mem/packet.hh"
#include "mem/request.hh"
#include "params/ARTCache.hh"
#include "sim/cur_tick.hh"

namespace gem5
{

/**
 * A model of the STM32 ART (Adaptive Real-Time) instruction
 * accelerator, implemented as a NoncoherentCache subclass.
 */
class ART : public NoncoherentCache
{
  public:
    ART(const ARTCacheParams &p);

  private:
    /** Bypass the tag store and forward every request to memory. */
    const bool directMemoryMode;

    /** Enable the two-buffer sequential prefetcher. */
    const bool enablePrefetch;

    /** Issue an allocated prefetch immediately on an I-Cache hit. */
    const bool prefetchOnCacheHit;

    /** Block size used for prefetch alignment (must be power of 2). */
    const unsigned pfBlkSize;

    /** Monotonically increasing order counter for queue entries. */
    Counter artPrefetchOrder;

    /** Requestor ID registered for ART prefetch traffic. */
    RequestorID artRequestorId;

    /**
     * Latency (in ticks) for serving from the ART prefetch / current
     * buffer (Cases B1 and B2 in the dispatch table).  Distinct from
     * ``portAhbBufferLatency``, which covers the port-level AHB-Lite
     * back-to-back pipeline buffer (Case A).
     *
     * This latency is ONLY used when ``enablePrefetch`` is true.  It
     * MUST NOT appear on any path active in the bypass mode
     * (``directMemoryMode && !enablePrefetch``).
     */
    const Tick prefetchBufferHitLatency;

    /**
     * Flash access latency for ART's case-F direct dispatch path.
     * ART models the flash access internally — functional read of
     * flash data + scheduled internal response — to avoid the timing
     * overhead of going through the standard memSidePort → flash_bus
     * XBar → SimpleMemory event machinery.  See ``issueFlashRead``.
     * Must match the connected flash memory's latency parameter.
     */
    const Tick flashResponseLatency;

    /**
     * Size in bytes of the port-level AHB buffer. Mirrors the most
     * recently delivered line at the cpuSidePort. 0 disables the
     * AHB buffer entirely.
     */
    const unsigned portAhbBufferSize;

    /** Latency (in ticks) for an AHB-buffer hit at the port. */
    const Tick portAhbBufferLatency;

    /** Start of the flash memory region (prefetch bounds check). */
    const Addr flashStartAddr;

    /** End of the flash memory region, inclusive (prefetch bounds). */
    const Addr flashEndAddr;

    // ---------------------------------------------------------------
    //  Pipeline: arriveBuffer → outputBuffer
    // ---------------------------------------------------------------

    /** A packet queued in the arrive or output buffer. */
    struct DeferredPacket
    {
        PacketPtr pkt;
        Tick tick;
        /**
         * True if this packet is bound to the serializing in-flight
         * slot (artPfEntry / bypassCacheEntry). When this packet drains
         * from outputBuffer, processingInFlight is cleared. Pipelined
         * buffer/cache hits set this false and never gate the flag.
         */
        bool boundToInFlight;

        /**
         * Case-F deferred AHB-buffer update.  In the PSM-internal
         * dispatch path (``issueFlashRead``), the AHB-port buffer
         * must be filled at the response-delivery tick (in
         * ``popOutputBuffer``), NOT at the dispatch tick — otherwise
         * within-line follow-up requests AHB-HIT before the flash
         * response is even delivered, producing PSM-incompatible
         * timing (ART would dispatch the next miss 5000 ticks before
         * PSM does).  These fields carry the line data from
         * ``issueFlashRead`` to ``popOutputBuffer`` for that fill.
         * ``hasLineData`` is left false for all other entries (Case A
         * AHB-hit serve, Cases B/C/D/E responses).
         */
        bool hasLineData = false;
        Addr lineBlockAddr = 0;
        uint32_t lineStreamId = ~uint32_t(0);
        std::vector<uint8_t> lineData;

        DeferredPacket(PacketPtr _pkt, Tick _tick,
                       bool _bound = false)
            : pkt(_pkt), tick(_tick), boundToInFlight(_bound) {}
    };

    /** Incoming requests waiting for address-phase processing. */
    std::deque<DeferredPacket> arriveBuffer;

    /** Max entries in arriveBuffer (0 = unlimited). */
    size_t arriveBufferSizeLimit;

    /**
     * Requests promoted past the address phase, awaiting buffer/cache/
     * flash dispatch.  Stage 2 of the PSM-equivalence refactor introduces
     * this buffer between arriveBuffer and the dispatcher — popArriveBuffer
     * moves one entry from arriveBuffer to readyToFireBuffer, then
     * popReadyToFireBuffer chooses Case A (AHB-port buffer hit), Case
     * B1/B2 (curBuf/pfBuf hit), or falls through to the serializing
     * cache/bypass dispatch.  Stage 3+ raises the size limit to enable
     * AHB-Lite back-to-back pipelining.
     */
    std::deque<DeferredPacket> readyToFireBuffer;

    /**
     * Max entries in readyToFireBuffer (0 = unlimited).  Stage 2 keeps
     * this at 1 so behavior matches the legacy single-slot pipeline;
     * Stage 3+ raises it.
     */
    size_t readyToFireBufferSizeLimit;

    /** Responses ready to be sent to upstream. */
    std::list<DeferredPacket> outputBuffer;

    /**
     * Set when a serializing request (cases 4/5/7/8 in the design doc:
     * artPfEntry alloc, CPU-waiting, direct-memory bypass) is in flight.
     * Cleared when the bound response drains from outputBuffer.
     * Pipelined buffer/cache hits do NOT set this flag.
     */
    bool processingInFlight = false;

    // ---- Stage-1 (issue 2026-05-10-art-bypass-vs-no-art-divergence)
    // pipeline-slot scaffolding -------------------------------------
    /**
     * Stage of an in-flight request.  Used by inFlightSlots to track
     * progress of multiple concurrent requests (AHB-Lite back-to-back).
     * Stage 1 only uses Empty / DataPhase as a parallel counter to
     * processingInFlight; later stages will use the full enum.
     */
    enum class InFlightStage : uint8_t
    {
        Empty = 0,
        Admitted,        // queued in arriveBuffer
        AddressPhase,    // address-phase event scheduled
        DataPhase,       // dispatched downstream (memory / cache)
        Response,        // waiting in outputBuffer for delivery
    };

    /** A single in-flight request slot.  See inFlightSlots. */
    struct InFlightSlot
    {
        PacketPtr pkt = nullptr;     // owning packet (or nullptr if free)
        Tick admitTick = 0;          // when admitted (DPRINTFs)
        uint32_t streamId = 0;       // for streamId flush bookkeeping
        InFlightStage stage = InFlightStage::Empty;
    };

    /**
     * In-flight request slots.  Capacity = maxOutstandingRequests.
     * Stage 1 wires this in parallel with processingInFlight (1:1 when
     * max_outstanding_requests == 1, the default-on-Stage-1 setting);
     * Stage 2+ shifts dispatch gating from processingInFlight to the
     * per-slot tracking.
     */
    std::vector<InFlightSlot> inFlightSlots;

    /** Acquire a free slot (returns slot index, or -1 if none free). */
    int slotAcquire(PacketPtr pkt, InFlightStage stage);

    /** Release the slot at the given index. */
    void slotRelease(int idx);

    /** Number of unallocated slots. */
    unsigned slotsAvailable() const;

    /** Find the slot index whose pkt matches; -1 if none. */
    int slotFindByPkt(PacketPtr pkt) const;

    /** Find the lowest-index slot in DataPhase (FIFO release order). */
    int slotFirstInDataPhase() const;

    /** Compact one-line slot state dump, for ARTPipeline DPRINTFs. */
    std::string slotsSnapshot() const;
    // ---- end Stage-1 scaffolding ---------------------------------

    /**
     * Pipeline buffer/cache hits at addressPhaseLatency cadence
     * instead of serializing on processingInFlight.
     */
    const bool enablePipeline;

    /**
     * Max number of in-flight requests (AHB-Lite back-to-back depth).
     * 1 = strict serialization; 2 = address-phase / data-phase
     * pipelining (the AHB-Lite spec).  Stage 0 wires this in but
     * does not yet act on it; Stage 3+ uses it.
     */
    const unsigned maxOutstandingRequests;

    /**
     * If true, when direct_memory_mode AND !enable_prefetch, route
     * via the PSM-equivalent fast path (no cache fill, no buffer
     * touches, no prefetch).  Stage 0 wires this in but does not
     * yet act on it; Stage 3 uses it.
     */
    const bool psmCompatibleBypass;

    /** AHB address phase duration (in ticks). */
    const Tick addressPhaseLatency;

    /** Last stream ID seen (for AHB transfer retraction). */
    uint32_t lastStreamId;

    /**
     * Stream ID tagged onto the AHB-port buffer's currently-held line.
     * Mirrors PSM's ``portBufferedStreamId``: when the next CPU request
     * carries a streamId, the AHB-buffer hit check requires both the
     * block address AND the streamId to match.  This models the silicon
     * behaviour where a taken-branch redirect (which bumps streamId in
     * the F-stage) invalidates the AHB sense-amp buffer for the new
     * stream.
     *
     * ``~0u`` means "no streamId tagged" — falls back to address-only
     * matching for untagged requests (e.g. DCode loads, system-port
     * functional accesses).
     */
    uint32_t ahbBufferedStreamId = ~uint32_t(0);

    /** Stale packets to return as BadAddress on stream change. */
    std::list<PacketPtr> staleReturnQueue;

    /** Bit 3 of BaseCache::blocked bitmask for arriveBuffer full. */
    static constexpr uint8_t BLOCKED_ARRIVE_FULL = (1 << 3);

    /** Pop one entry from arriveBuffer and process it. */
    void popArriveBuffer();
    EventFunctionWrapper popArriveBufferEvent;

    /** Legacy (serialized) request handler: one request at a time. */
    void popArriveBufferSerialized();

    /** New pipelined request handler: see design plan. */
    void popArriveBufferPipelined();

    /**
     * Drain the readyToFireBuffer at the dispatch point.  Mirrors PSM's
     * popReadyToFireBuffer (gem5/src/mem/pipelined_simple_mem.cc).
     * Order: stream-id flush (Case G), Case A (AHB-port buffer hit
     * fast path; never falls through to cache/prefetch), Case B1/B2
     * (curBuf/pfBuf hit), then AHB-miss fallback to the serializing
     * dispatch (Case C/D/E/F).  Stage 2 introduces the structural
     * separation; Stage 3 will replace the AHB-miss fallback with the
     * PSM-equivalent direct flash path.
     */
    void popReadyToFireBuffer();

    /**
     * Update nextPfAddr based on @p reqAddr and, if needed, allocate
     * the artPfEntry to issue a sequential prefetch. Extracted from
     * the original popArriveBuffer for reuse by both serialized and
     * pipelined paths.
     *
     * @param reqAddr        The address of the just-served request.
     * @param prefetchHit    Whether the request was served from a buffer.
     * @param schedImmediate When true, schedule the prefetch send event
     *                       at clockEdge() (used when the cache won't
     *                       trigger it later).
     */
    void maybeIssueNextPrefetch(Addr reqAddr, bool prefetchHit,
                                bool schedImmediate);

    /** Drain outputBuffer: send responses to upstream. */
    void popOutputBuffer();
    EventFunctionWrapper popOutputBufferEvent;

    /**
     * Queue a response for sending from the output stage.
     * @param boundToInFlight  See DeferredPacket::boundToInFlight.
     */
    void pushToOutputBuffer(PacketPtr pkt, Tick readyTick,
                            bool boundToInFlight = false);

    /** Emit one row of the ARTCacheLineTrace table for @p event. */
    void lineTraceEvent(const char *event, Addr addr,
                        const char *cls = "");

    /** Schedule popOutputBufferEvent if outputBuffer has entries. */
    void scheduleOutputDrain();

    /**
     * Schedule next popArriveBuffer if arriveBuffer is non-empty,
     * processingInFlight is false, and event not already scheduled.
     */
    void scheduleNextArriveBuffer();

    /** Unblock the port if arriveBuffer is below capacity. */
    void maybeUnblockArriveBuffer();

    /** Block the port because arriveBuffer is full. */
    void blockForArriveBuffer();

    /** Unblock the port (arriveBuffer drained below capacity). */
    void unblockForArriveBuffer();

    /** Check if port is blocked due to arriveBuffer full. */
    bool isBlockedForArriveBuffer() const;

    /** Retry sending stale (bad-address) packets from stream change. */
    void retryStaleReturn();
    EventFunctionWrapper staleReturnEvent;

  protected:
    QueueEntry* getNextQueueEntry() override;
    Tick nextQueueReadyTime() const override;
    void recvTimingReq(PacketPtr pkt) override;
    void recvTimingResp(PacketPtr pkt) override;
    void serviceMSHRTargets(MSHR *mshr, const PacketPtr pkt,
                            CacheBlk *blk) override;
    void handleTimingReqHit(PacketPtr pkt, CacheBlk *blk,
                            Tick request_time) override;

    /**
     * Check whether the addressed block is already present and valid
     * in the tag store.
     *
     * @param pkt  Packet whose address is looked up.
     * @return true if the block is valid in the cache.
     */
    bool isDataInCache(PacketPtr pkt);

    /**
     * Insert prefetch response data into the I-Cache tag store.
     *
     * Per [RM0440 Section 3.3.4]: "Each time a miss occurs (requested
     * data not present in the currently used instruction line, in the
     * prefetched instruction line or in the instruction cache memory),
     * the line read is copied into the instruction cache memory."
     *
     * This is called when a prefetch response arrives and the CPU was
     * waiting for it (i.e., the CPU missed all three: curBuf, pfBuf,
     * and I-Cache).  The data is written into the NoncoherentCache tag
     * store so that future accesses to the same address can hit the
     * I-Cache directly.
     *
     * @param pkt  The prefetch response packet (block-aligned, blkSize
     *             bytes, with valid data).  Must NOT be deleted before
     *             this call.
     * @return true if the data was successfully inserted into the cache.
     */
    bool fillCacheWithPrefetchData(PacketPtr pkt);

  public:

    /** Return hex string representation of an address (for DPRINTF). */
    std::string addrToString(Addr addr) const
    {
        std::stringstream ss;
        ss << std::hex << addr;
        return ss.str();
    }

    /** Align @p addr down to the prefetch block boundary. */
    Addr convertAddrToPfBlockAddr(Addr addr) const
    {
        return addr & ~(Addr(pfBlkSize) - 1);
    }

    /** Return the block-aligned address immediately after @p addr. */
    Addr getNextSequentialAddr(Addr addr) const
    {
        return convertAddrToPfBlockAddr(addr) + pfBlkSize;
    }

    /**
     * Check whether @p addr falls within the configured flash region.
     *
     * @return true if addr is inside [flashStartAddr, flashEndAddr].
     */
    bool isInFlashRange(Addr addr) const
    {
        return addr >= flashStartAddr && addr <= flashEndAddr;
    }

    const uint8_t* getSubBlockData(const PacketPtr src, const PacketPtr dest) {
        Addr offset = dest->getAddr() - src->getAddr();
        assert(offset + dest->getSize() <= src->getSize());
        return src->getConstPtr<uint8_t>() + offset;
    }

  protected:
    // ---------------------------------------------------------------
    //  Prefetch buffer
    // ---------------------------------------------------------------

    /**
     * A simple buffer that holds one prefetch-block-sized line of data.
     *
     * Two instances are used:
     *  - prefetchBuffer: the line being or just prefetched from memory.
     *  - currentBuffer:  the previously prefetched line, available for
     *                    immediate CPU hits.
     */
    struct ARTPrefetchBuffer
    {
        /** Block-aligned address of the stored line. */
        Addr addr;

        /** Size of the prefetch block in bytes. */
        unsigned blkSize;

        /** Whether this buffer contains valid data. */
        bool valid;

        /** Raw data storage. */
        std::vector<uint8_t> data;

        ARTPrefetchBuffer(unsigned blk_size)
            : addr(0), blkSize(blk_size), valid(false)
        {
            data.resize(blk_size, 0);
        }

        /**
         * Store a line into this buffer.
         *
         * @param src_addr  Block-aligned source address.
         * @param src_data  Pointer to at least blkSize bytes of data.
         */
        void storeData(Addr src_addr, const uint8_t *src_data)
        {
            assert(data.size() == blkSize);
            std::memcpy(data.data(), src_data, blkSize);
            addr = src_addr;
            valid = true;
        }

        /**
         * Check whether @p query_addr falls within this buffer's block.
         * The query address is aligned to the block size before comparison.
         */
        bool isHit(Addr query_addr) const
        {
            return valid &&
                   ((query_addr & ~(Addr(blkSize) - 1)) == addr);
        }

        void invalidate() { valid = false; }
        bool isValid() const { return valid; }

        /** Deep-copy another buffer's content into this one. */
        void copyFrom(const ARTPrefetchBuffer &other)
        {
            addr = other.addr;
            blkSize = other.blkSize;
            valid = other.valid;
            data = other.data;
        }

        const uint8_t* getData(Addr target_addr, unsigned target_size) {
            Addr offset = target_addr - addr;
            assert(offset + target_size <= blkSize);
            uint8_t *buf_data = data.data() + offset;
            return buf_data;
        }

    };

    /** Holds the line being or just prefetched from memory. */
    ARTPrefetchBuffer prefetchBuffer;

    /** Holds the previously prefetched line available for CPU hits. */
    ARTPrefetchBuffer currentBuffer;

    /**
     * Port-level AHB buffer.
     *
     * Mirrors the most recently delivered line at the cpuSidePort
     * (via curBuf hit, pfBuf hit, cache hit, or flash response).
     * Subsequent CPU requests whose address falls within this line
     * are served at portAhbBufferLatency (one AHB-Lite data-phase),
     * pipelining with the next request's address phase. This models
     * the AHB-Lite back-to-back transfer behavior shown in
     * RM0440 Figure 3 (sequential 16-bit + prefetch, 3 WS).
     *
     * Disabled when portAhbBufferSize == 0.
     */
    ARTPrefetchBuffer ahbBuffer;

    /** Next sequential address to prefetch. */
    Addr nextPfAddr;

    /**
     * Serve a CPU request directly from a prefetch buffer.
     *
     * Copies data into @p pkt, converts it to a timing response, and
     * schedules the response on the CPU-side port.
     *
     * @param pkt  The CPU request packet.
     * @param buf  The prefetch buffer to serve from.
     */
    void serveFromBuffer(PacketPtr pkt, ARTPrefetchBuffer &buf);

    /**
     * Update the port-level AHB buffer with the line just delivered.
     * Called after every successful serve path (curBuf, pfBuf, cache
     * hit, prefetch response, bypass response). No-op when the AHB
     * buffer is disabled (portAhbBufferSize == 0).
     *
     * @param src       The buffer whose data is being delivered to the CPU.
     * @param streamId  Stream ID to tag onto the AHB buffer
     *                  (``~0u`` = no streamId / untagged).
     */
    void updateAHBBufferFromLine(const ARTPrefetchBuffer &src,
                                 uint32_t streamId = ~uint32_t(0));

    /**
     * Update the port-level AHB buffer from a raw response packet.
     * Used when the line came from flash/cache (not from one of the
     * ART buffers).
     *
     * @param blk_addr  Block-aligned source address.
     * @param src_data  Pointer to at least portAhbBufferSize bytes.
     * @param streamId  Stream ID to tag onto the AHB buffer
     *                  (``~0u`` = no streamId / untagged).
     */
    void updateAHBBufferFromRaw(Addr blk_addr, const uint8_t *src_data,
                                uint32_t streamId = ~uint32_t(0));

    /**
     * Serve a CPU request from the AHB buffer. Pushes a response to
     * outputBuffer at +portAhbBufferLatency (AHB data-phase). Caller
     * has already verified ahbBuffer.isHit(pkt->getAddr()).
     */
    void serveFromAHBBuffer(PacketPtr pkt);

    // ---------------------------------------------------------------
    //  Prefetch queue entry
    // ---------------------------------------------------------------

    /**
     * A QueueEntry that tracks a single outstanding ART prefetch
     * request.  It leverages BaseCache's scheduling and retry
     * mechanism.
     */
    class ARTPfQueueEntry : public QueueEntry, public Printable
    {
      public:
        ARTPfQueueEntry(const std::string &name, unsigned blk_size)
            : QueueEntry(name),
              hasTarget(false),
              cpuWaiting(false),
              retrying(false),
              cpuPtr(nullptr)
        {
            fatal_if(blk_size == 0,
                     "ART prefetch block size must be non-zero");
            blkSize = blk_size;
        }

        bool matchBlockAddr(const Addr addr,
                            const bool is_secure) const override
        {
            return ((addr & ~(Addr(blkSize) - 1)) == blkAddr) &&
                                                    (isSecure == is_secure);
        }

        bool matchBlockAddr(const PacketPtr pkt) const override
        {
            return ((pkt->getAddr() & ~(Addr(blkSize) - 1)) == blkAddr) &&
                                                (isSecure == pkt->isSecure());
        }

        bool conflictAddr(const QueueEntry *entry) const override
        {
            return entry->matchBlockAddr(blkAddr, isSecure);
        }

        bool sendPacket(BaseCache &cache) override
        {
            ART *artCache = dynamic_cast<ART *>(&cache);
            assert(artCache);
            return artCache->sendARTPrefetchPacket(this);
        }

        /**
         * Allocate this entry for a new prefetch request.
         *
         * @param blk_addr   Block-aligned target address.
         * @param target     The prefetch packet.
         * @param when_ready Tick when the entry becomes serviceable.
         * @param _order     Global ordering counter.
         */
        void allocate(Addr blk_addr, PacketPtr target, Tick when_ready,
                      Counter _order)
        {
            blkAddr = blk_addr;
            isSecure = false;
            order = _order;
            readyTime = when_ready;
            assert(target);
            inService = false;
            _isUncacheable = false;
            pfTarget = std::make_shared<Target>(target, when_ready, _order);
            hasTarget = true;
        }

        void deallocate()
        {
            pfTarget = nullptr;
            inService = false;
            hasTarget = false;
        }

        bool ready() const { return hasTarget && !inService; }
        bool ifHasTarget() const { return hasTarget; }

        bool markInService()
        {
            if (!hasTarget || inService)
                return false;
            inService = true;
            return true;
        }

        bool ifInService() const { return inService; }

        /**
         * Record that the CPU is waiting for this prefetch to complete.
         * @return false if the CPU is already waiting.
         */
        bool setCPUWaiting(PacketPtr pkt)
        {
            if (cpuWaiting)
                return false;
            cpuWaiting = true;
            cpuPtr = pkt;
            return true;
        }

        bool clearCPUWaiting()
        {
            if (!cpuWaiting)
                return false;
            cpuWaiting = false;
            cpuPtr = nullptr;
            return true;
        }

        bool ifCPUWaiting() const { return cpuWaiting; }

        Target *getTarget() override
        {
            assert(hasTarget);
            return pfTarget.get();
        }

        void print(std::ostream &os,
                   int verbosity = 0,
                   const std::string &prefix = "") const override
        {
            ccprintf(os, "%s[%#llx:%#llx](%s) %s %s %s %s %s %s\n",
                     prefix, blkAddr, blkAddr + blkSize - 1,
                     isSecure ? "s" : "ns",
                     _isUncacheable ? "Unc" : "",
                     inService ? "InSvc" : "",
                     hasTarget ? "HasTgt" : "",
                     cpuWaiting ? "CpuWait" : "",
                     retrying ? "Retry" : "",
                     ready() ? "Ready" : "");

            if (pfTarget) {
                ccprintf(os, "%s  Targets:\n", prefix);
                ccprintf(os, "%s    FromARTPrefetcher: ", prefix);
                pfTarget->pkt->print(os, verbosity, "");
            }
        }

        std::string print() const
        {
            std::ostringstream str;
            print(str);
            return str.str();
        }

        bool ifRetrying() const { return retrying; }
        void markRetrying() { retrying = true; }
        void clearRetrying() { retrying = false; }

      private:
        std::shared_ptr<Target> pfTarget;
        bool hasTarget;
        bool cpuWaiting;
        bool retrying;

      public:
        /** Packet the CPU is waiting on (valid when cpuWaiting). */
        PacketPtr cpuPtr;
    };

    ARTPfQueueEntry artPfEntry;

    bool sendARTPrefetchPacket(ARTPfQueueEntry *entry);

    /**
     * Create a read packet for a prefetch request.
     *
     * @param request       The request to wrap.
     * @param sender_state  Sender state to push onto the packet.
     * @return A new ReadReq packet with allocated data storage.
     */
    PacketPtr makeARTPrefetchPacket(const RequestPtr &request,
                                    Packet::SenderState *sender_state)
    {
        PacketPtr ret = new Packet(request, MemCmd::ReadReq);
        ret->pushSenderState(sender_state);
        ret->allocate();
        return ret;
    }

    /**
     * Create a prefetch Request with the ART-specific requestor ID.
     *
     * @param addr      Block-aligned target address.
     * @param blk_size  Block size in bytes.
     * @return A shared Request with PREFETCH flag set.
     */
    RequestPtr makeARTPrefetchRequest(Addr addr, unsigned blk_size)
    {
        RequestPtr req = std::make_shared<Request>(
            addr, blk_size, 0, artRequestorId
        );
        req->setFlags(Request::PREFETCH);
        return req;
    }

    // ---------------------------------------------------------------
    //  Bypass cache entry (direct-memory mode)
    // ---------------------------------------------------------------

    /**
     * A single-entry queue used when direct-memory mode is enabled.
     *
     * The entry tracks a CPU request that bypasses the tag store and
     * is forwarded straight to memory.  Two target slots are used:
     *  - bypassTarget:  the request being prepared / waiting for send.
     *  - waitingTarget:  the request that has been sent and is
     *                    awaiting a response.
     */
    class BypassCacheEntry : public QueueEntry
    {
      public:
        BypassCacheEntry(const std::string &name)
            : QueueEntry(name), hasBypassTarget(false),
              hasWaitingTarget(false), retrying(false) {}

        bool matchBlockAddr(const PacketPtr pkt) const override
        {
            return false;
        }

        bool matchBlockAddr(const Addr addr,
                            const bool is_secure) const override
        {
            return false;
        }

        bool conflictAddr(const QueueEntry *entry) const override
        {
            return false;
        }

        bool sendPacket(BaseCache &cache) override
        {
            ART *artCache = dynamic_cast<ART *>(&cache);
            assert(artCache);
            return artCache->sendBypassPacket(this);
        }

        /**
         * Allocate this entry for a new bypass request.
         *
         * @param target     The CPU request packet.
         * @param when_ready Tick when the entry becomes serviceable.
         * @param _order     Global ordering counter.
         */
        void allocate(PacketPtr target, Tick when_ready, Counter _order)
        {
            assert(target);
            blkAddr = target->getAddr();
            order = _order;
            readyTime = when_ready;
            inService = false;
            _isUncacheable = false;
            target->pushSenderState(this);
            bypassTarget = std::make_unique<Target>(
                target, when_ready, _order);
            hasBypassTarget = true;
        }

        void deallocate()
        {
            bypassTarget.reset();
            hasBypassTarget = false;
            waitingTarget.reset();
            hasWaitingTarget = false;
            inService = false;
        }

        Target *getTarget() override
        {
            return bypassTarget.get();
        }

        void markInService()
        {
            assert(!hasWaitingTarget && hasBypassTarget);
            inService = true;
            waitingTarget = std::move(bypassTarget);
            hasBypassTarget = false;
            hasWaitingTarget = true;
        }

        bool ifTargetOpen() const { return !hasBypassTarget; }
        bool ifInService() const { return inService; }
        bool ready() const { return hasBypassTarget && !inService; }

        bool ifRetrying() const { return retrying; }
        void markRetrying() { retrying = true; }
        void clearRetrying() { retrying = false; }

      private:
        bool hasBypassTarget;
        bool hasWaitingTarget;
        bool retrying;
        std::unique_ptr<Target> bypassTarget;
        std::unique_ptr<Target> waitingTarget;
    };

    BypassCacheEntry bypassCacheEntry;

    bool sendBypassPacket(BypassCacheEntry *entry);

    class ARTTranslateState : public Packet::SenderState
    {
    public:
        PacketPtr cpuPkt;  // the original 4-byte packet to serve later
        ARTTranslateState(PacketPtr pkt) : cpuPkt(pkt) {}
    };

    ARTTranslateState *cachePktEntry = nullptr;

    /**
     * Stage-3 PSM-equivalent bypass sender state (issue
     * 2026-05-10-art-bypass-vs-no-art-divergence).  Carries the
     * original CPU packet and the in-flight-slot index through the
     * flash round-trip so ``recvTimingResp`` can recover them and
     * release the slot.
     *
     * This sender state replaces the bypassCacheEntry queue path for
     * case F (``directMemoryMode && !enablePrefetch &&
     * psmCompatibleBypass``): the packet goes straight from
     * ``popReadyToFireBuffer`` to ``memSidePort.sendTimingReq`` with
     * no ``schedMemSideSendEvent`` indirection, and the response is
     * delivered at ``curTick()`` with no ``clockEdge(Cycles(1))``
     * realignment.  Eliminating both clockEdge realignments closes
     * the 2-cycle/miss gap vs PSM.
     */
    class ARTPSMBypassState : public Packet::SenderState
    {
    public:
        PacketPtr cpuPkt;
        int slotIdx;
        ARTPSMBypassState(PacketPtr p, int idx)
            : cpuPkt(p), slotIdx(idx) {}
    };

    /**
     * Stage 3: direct flash dispatch for case F.  Builds a
     * pfBlkSize-aligned read packet, tags it with
     * ``ARTPSMBypassState``, and calls
     * ``memSidePort.sendTimingReq`` directly.  Caller has already
     * called ``slotAcquire``; this method transitions the slot to
     * ``DataPhase``.
     */
    void issueFlashRead(PacketPtr pkt, int slotIdx);

    // ---------------------------------------------------------------
    //  Statistics
    // ---------------------------------------------------------------

    struct ARTStats : public statistics::Group
    {
        ARTStats(ART &art);

        statistics::Scalar currentBufferHits;
        statistics::Scalar prefetchBufferHits;
        statistics::Scalar prefetchMisses;
        statistics::Scalar bypassAccesses;
        statistics::Scalar cpuWaitEvents;
        statistics::Scalar prefetchesAllocated;
        statistics::Scalar prefetchesIssued;
        statistics::Scalar prefetchesCompleted;
        statistics::Scalar prefetchCacheFills;
        statistics::Scalar prefetchCacheFillSkips;
    } stats;
};

} // namespace gem5

#endif // __MEM_CACHE_ART_HH__
