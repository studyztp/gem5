#ifndef __MEM_CACHE_ART_HH__
#define __MEM_CACHE_ART_HH__

#include "base/printable.hh"
#include "base/trace.hh"
#include "base/types.hh"
#include "debug/Cache.hh"
#include "debug/ARTCache.hh"
#include "mem/cache/cache.hh"          // for Cache
#include "mem/cache/queue_entry.hh"    // for QueueEntry
#include "mem/packet.hh"
#include "mem/request.hh"
#include "sim/cur_tick.hh"
#include "params/ARTCache.hh" 

#include <string>
#include <vector>
#include <sstream>

namespace gem5
{

class ART : public Cache
{
  public:
    ART(const ARTCacheParams &p);

  private:
    bool bypassCache;
    bool bypassPrefetch;
    unsigned pfBlkSize;
    bool prefetch_hit;

  protected:
    QueueEntry* getNextQueueEntry();
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
            isSecure = target->isSecure();
            order = _order;
            readyTime = when_ready;
            assert(target);
            inService = false;
            _isUncacheable = target->req->isUncacheable();
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

        bool markInService()
        {
            if (!hasTarget || inService) {
                return false;
            }
            inService = true;
            return true;
        }

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

      private:
        std::shared_ptr<Target> pfTarget;
        // TODO: think about if making this a unique_ptr is better
        bool hasTarget;

    };

    ARTPfQueueEntry artPfEntry;

    bool sendARTPrefetchPacket(ARTPfQueueEntry* entry);

    PacketPtr makeARTPrefetchPacket(const RequestPtr &request,
        Packet::SenderState *sender_state) {
            PacketPtr ret = Packet::createRead(request);
            assert(sender_state);
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
};
} // namespace gem5
#endif // __MEM_CACHE_ART_HH__