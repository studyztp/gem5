#ifndef __MEM_CACHE_ART_HH__
#define __MEM_CACHE_ART_HH__

#include "base/printable.hh"
#include "base/trace.hh"
#include "base/types.hh"
#include "debug/Cache.hh"
#include "debug/ARTCache.hh"
#include "mem/cache/noncoherent_cache.hh" // for NoncoherentCache
#include "mem/cache/queue_entry.hh"       // for QueueEntry
#include "mem/packet.hh"
#include "mem/request.hh"
#include "sim/cur_tick.hh"
#include "params/ARTCache.hh" 

#include <string>
#include <vector>
#include <queue>
#include <sstream>

namespace gem5
{

class ART : public NoncoherentCache
{
  public:
    ART(const ARTCacheParams &p);

  private:
    bool bypassCache;
    bool bypassPrefetch;
    unsigned pfBlkSize;
    bool prefetch_hit;
    Counter artPrefetchOrder;

  protected:
    QueueEntry* getNextQueueEntry() override;
    Tick nextQueueReadyTime() const override;
    void recvTimingReq(PacketPtr pkt) override;
    void recvTimingResp(PacketPtr pkt) override;
    bool ifDataInCache(PacketPtr pkt);

  public:
    // helper function for printing address
    std::string addrToString(Addr addr) const
    {   
        std::stringstream ss;
        ss << std::hex << addr;
        return ss.str();
    }

    Addr convertAddrToPfBlockAddr(Addr addr) const
    {
        return addr & ~(pfBlkSize - 1);
    }

    Addr getNextSequentialAddr(Addr addr) const
    {
        return convertAddrToPfBlockAddr(addr) + pfBlkSize;
    }

  protected:
    // ART prefetch elements

    struct ARTPrefetchBuffer{
        Addr addr;
        unsigned blk_size;
        bool valid;
        std::vector<uint8_t> data;

        ARTPrefetchBuffer(unsigned blk_size)
            : addr(0), blk_size(blk_size), valid(false)
            {
                // initialize data buffer
                data.resize(blk_size, 0);
            }
        
        void storeData(Addr src_addr, const uint8_t* src_data) {
            // only store blk_size bytes
            assert(data.size() == blk_size);
            std::memcpy(data.data(), src_data, blk_size);
            addr = src_addr;
            valid = true;
        }

        bool isHit(Addr query_addr) const {
            return valid && (addr == query_addr);
        }

        void invalidate() {
            valid = false;
        }

        bool isValid() const {
            return valid;
        }

        void copyFrom(const ARTPrefetchBuffer& other) {
            addr = other.addr;
            blk_size = other.blk_size;
            valid = other.valid;
            data = other.data;
        }
    };

    ARTPrefetchBuffer prefetchBuffer;
    // prefetch buffer store the currently prefetching instruction line
    ARTPrefetchBuffer currentBuffer;
    // current buffer store the instruction line that is prefetched
    Addr nextPfAddr;
    // next sequential address to prefetch

    // ART prefetch needs a QueueEntry object to schedule the prefetch request
    class ARTPfQueueEntry : public QueueEntry, public Printable
    {

      public:

        ARTPfQueueEntry(
            const std::string &name,
            unsigned blk_size
        ): QueueEntry(name)
        {
            assert(blk_size != 0);
            blkSize = blk_size;
            hasTarget = false;
            cpuWaiting = false;
            cpuPtr = nullptr;
            retrying = false;
            // initialize other members if needed
        }
       
        // define virtual functions
        bool matchBlockAddr(
            const Addr addr,
            const bool is_secure) const override
        {
            return (blkAddr == addr) && (isSecure == is_secure);
        }

        bool matchBlockAddr(const PacketPtr pkt) const override
        {
            return pkt->matchBlockAddr(blkAddr, isSecure, blkSize);
        }

        bool conflictAddr(const QueueEntry* entry) const override
        {
            return entry->matchBlockAddr(blkAddr, isSecure);
        }

        bool sendPacket(BaseCache &cache) override
        {
            ART* artCache = dynamic_cast<ART*>(&cache);
            assert(artCache);
            return artCache->sendARTPrefetchPacket(this);
        }

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

        bool ready() const { return hasTarget&&!inService; }
        bool ifHasTarget() const { return hasTarget; }
        bool markInService()
        {
            if (!hasTarget || inService) {
                return false;
            }
            inService = true;
            return true;
        }

        bool ifInService() const { return inService; }

        bool setCPUWaiting(PacketPtr pkt) {
            if (cpuWaiting) {
                return false;
            }
            cpuWaiting = true;
            cpuPtr = pkt;
            return true;
        }

        bool clearCPUWaiting() {
            if (!cpuWaiting) {
                return false;
            }
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

        // Implement the print function for debugging
        void print(std::ostream &os,
               int verbosity = 0,
               const std::string &prefix = "") const override
        {
            ccprintf(os, "%s[%#llx:%#llx](%s) %s %s %s state: %s %s %s %s %s\n",
                        prefix, blkAddr, blkAddr + blkSize - 1,
                        isSecure ? "s" : "ns",
                        _isUncacheable ? "Unc" : "",
                        inService ? "InSvc" : "");

            ccprintf(os, "%s  Targets:\n", prefix);
            ccprintf(os, "%sFromARTPrefetcher: ", prefix);
            pfTarget->pkt->print(os, verbosity, "");
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
        // TODO: think about if making this a unique_ptr is better
        bool hasTarget;
        bool cpuWaiting;
        bool retrying;

      public:
        //   Keep it public for easy access for now
        PacketPtr cpuPtr;

    };

    ARTPfQueueEntry artPfEntry;

    bool sendARTPrefetchPacket(ARTPfQueueEntry* entry);

    PacketPtr makeARTPrefetchPacket(const RequestPtr &request,
        Packet::SenderState *sender_state) {
            PacketPtr ret = new Packet(request,MemCmd::ReadReq);
            ret->pushSenderState(sender_state);
            ret->allocate();
            return ret;
    }

    RequestPtr ARTPfRequest;

    RequestPtr makeARTPrefetchRequest(Addr addr, unsigned blk_size) {
        RequestPtr req = std::make_shared<Request>(
            addr, blk_size, 0, Request::funcRequestorId
        );
        req->setFlags(Request::PREFETCH);
        return req;
    }

    // Because we may bypass the cache, we need to handel the packets that
    // might require retry
    class BypassCacheEntry : public QueueEntry
    {
      public: 
        BypassCacheEntry(const std::string &name)
            : QueueEntry(name), hasByPassTarget(false),
              hasWaitingTarget(false), retrying(false), bypassTarget(nullptr),
              waitingTarget(nullptr) {}

        bool matchBlockAddr(const PacketPtr pkt) const override
        {
            return false; // Bypass entry does not match any block address
        }

        bool matchBlockAddr(
            const Addr addr,
            const bool is_secure) const override
        {
            return false; // Bypass entry does not match any block address
        }

        bool conflictAddr(const QueueEntry* entry) const override
        {
            return false; // Bypass entry does not conflict with any block address
        }

        bool sendPacket(BaseCache &cache) override
        {
            ART* artCache = dynamic_cast<ART*>(&cache);
            assert(artCache);
            return artCache->sendBypassPacket(this);
        }

        void allocate(PacketPtr target, Tick when_ready, Counter _order)
        {
            assert(target);
            order = _order;
            readyTime = when_ready;
            inService = false;
            _isUncacheable = false;
            target->pushSenderState(this);
            // We use sender state to see which packet is associated with
            // this bypass entry
            bypassTarget = new Target(target, when_ready, _order);
            hasByPassTarget = true;
        }

        void deallocate()
        {   
            delete waitingTarget;
            waitingTarget = nullptr;
            inService = false;
            hasWaitingTarget = false;
        }

        Target *getTarget() override
        {
            return bypassTarget;
        }

        void markInService()
        {
            assert(!hasWaitingTarget&&hasByPassTarget);
            inService = true;
            waitingTarget = bypassTarget;
            bypassTarget = nullptr;
            hasByPassTarget = false;
            hasWaitingTarget = true;
        }

        bool ifTargetOpen() const { return !hasByPassTarget; }
        bool ifInService() const { return inService; }
        bool ready() const { return hasByPassTarget&&!inService; }

        bool ifRetrying() const { return retrying; }
        void markRetrying() { retrying = true; }
        void clearRetrying() { retrying = false; }

      private:
        bool hasByPassTarget;
        bool hasWaitingTarget;
        bool retrying;
        Target* bypassTarget;
        Target* waitingTarget;
    };

    BypassCacheEntry bypassCacheEntry;

    bool sendBypassPacket(BypassCacheEntry* entry);
};
} // namespace gem5
#endif // __MEM_CACHE_ART_HH__