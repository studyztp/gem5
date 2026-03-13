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

#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "base/intmath.hh"
#include "base/printable.hh"
#include "base/statistics.hh"
#include "base/trace.hh"
#include "base/types.hh"
#include "debug/ARTCache.hh"
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

    /** Block size used for prefetch alignment (must be power of 2). */
    const unsigned pfBlkSize;

    /** Monotonically increasing order counter for queue entries. */
    Counter artPrefetchOrder;

    /** Requestor ID registered for ART prefetch traffic. */
    RequestorID artRequestorId;

    /** Start of the flash memory region (prefetch bounds check). */
    const Addr flashStartAddr;

    /** End of the flash memory region, inclusive (prefetch bounds). */
    const Addr flashEndAddr;

  protected:
    QueueEntry* getNextQueueEntry() override;
    Tick nextQueueReadyTime() const override;
    void recvTimingReq(PacketPtr pkt) override;
    void recvTimingResp(PacketPtr pkt) override;

    /**
     * Check whether the addressed block is already present and valid
     * in the tag store.
     *
     * @param pkt  Packet whose address is looked up.
     * @return true if the block is valid in the cache.
     */
    bool isDataInCache(PacketPtr pkt);

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
    };

    /** Holds the line being or just prefetched from memory. */
    ARTPrefetchBuffer prefetchBuffer;

    /** Holds the previously prefetched line available for CPU hits. */
    ARTPrefetchBuffer currentBuffer;

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
            : QueueEntry(name), hasTarget(false),
              cpuWaiting(false), cpuPtr(nullptr), retrying(false)
        {
            fatal_if(blk_size == 0,
                     "ART prefetch block size must be non-zero");
            blkSize = blk_size;
        }

        bool matchBlockAddr(const Addr addr,
                            const bool is_secure) const override
        {
            return (blkAddr == addr) && (isSecure == is_secure);
        }

        bool matchBlockAddr(const PacketPtr pkt) const override
        {
            return pkt->matchBlockAddr(blkAddr, isSecure, blkSize);
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
        statistics::Scalar prefetchesIssued;
        statistics::Scalar prefetchesCompleted;
    } stats;
};

} // namespace gem5

#endif // __MEM_CACHE_ART_HH__
