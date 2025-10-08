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

};
} // namespace gem5
#endif // __MEM_CACHE_ART_HH__