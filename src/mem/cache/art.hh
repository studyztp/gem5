#ifndef __MEM_CACHE_ART_HH__
#define __MEM_CACHE_ART_HH__

#include "base/printable.hh"
#include "base/trace.hh"
#include "base/types.hh"
#include "debug/Cache.hh"
#include "mem/cache/cache.hh"          // for Cache
#include "mem/cache/queue_entry.hh"    // for QueueEntry
#include "mem/packet.hh"
#include "mem/request.hh"
#include "sim/cur_tick.hh"

#include <string>
#include <vector>

namespace gem5
{

class ART : public Cache
{
  public:
    ART(const CacheParams &p);

    enum class ARTPrefetcherState {
        // States of the ART prefetcher to track its lifecycle and trigger
        // events appropriately
        EMPTY,
        INITIAL,
        WAITING,
        WAITING_N_CPU_RESERVED,
        WAITING_N_SCHEDULE_NEXT,
        READY
    };

    struct ARTPrefetchBuffer {
        // Simple ART buffer - just one entry
        Addr addr;
        std::vector<uint8_t> data;
        unsigned size;
        bool valid;
        Tick fetchTime;

        ARTPrefetchBuffer(unsigned blk_size) : 
          addr(0), valid(false), fetchTime(0) {
            // Initialize buffer to block size, block size in bytes
            size = blk_size;
            data.resize(size);
        }

        void store(Addr _addr, const uint8_t* _data) {
            addr = _addr;
            std::memcpy(data.data(), _data, size);
            valid = true;
            fetchTime = curTick();
        }
        
        void invalidate() { valid = false; }
        bool matches(Addr _addr) const {
            // If the buffer contains the block for the given address
            Addr blk_addr = _addr & ~(size - 1);  // Block align the address
            return valid && (addr == blk_addr);
        }
        
        void copyData(PacketPtr pkt) const {
            assert(valid);
            pkt->setDataFromBlock(data.data(), size);
        }
        Addr getNextAddr(Addr current_addr) const {
            Addr blk_addr = current_addr & ~(size - 1);
            return blk_addr + size;  // Next sequential block
        }
    };

  protected:

    QueueEntry* getNextQueueEntry() override;
    bool recvTimingReq(PacketPtr pkt) override;
    void recvTimingResp(PacketPtr pkt) override;
    bool ifDataInCache(PacketPtr pkt);
  
    class ARTPrefetcherEntry :
        public QueueEntry,
        public Printable
    {
        // In order to avoid implicit conversions, create an ART prefetcher
        // entry to define the prefetcher state and related methods so we can
        // use the QueueEntry mechanisms that gem5 BaseCache provides.
        template<typename Entry>

      public:
        using QueueEntry::Target;

        explicit ARTPrefetcherEntry(const std::string &name)
          : QueueEntry(name) {}

        bool isSecure() const override { return isSecure; }

        // QueueEntry virtuals
        bool sendPacket(BaseCache &cache) override;
        bool matchBlockAddr(Addr addr, bool is_secure) const override;
        bool matchBlockAddr(PacketPtr pkt) const override;
        bool conflictAddr(const QueueEntry* entry) const override;

        void allocate(Addr blk_addr, unsigned blk_size, PacketPtr orig_pkt,
                      Tick when_ready, Counter order);
        void deallocate();

        // Functional probing
        bool trySatisfyFunctional(PacketPtr pkt);

        // State helpers
        bool isReady() const { return state == ARTPrefetcherState::READY; }
        bool isWaiting() const { return state == ARTPrefetcherState::WAITING; }
        bool isInitial() const { return state == ARTPrefetcherState::INITIAL; }
        bool isEmpty() const { return state == ARTPrefetcherState::EMPTY; }
        bool isWaitingNCPUReserved() const {
            return state == ARTPrefetcherState::WAITING_N_CPU_RESERVED;
        }
        bool isWaitingNScheduleNext() const {
            return state == ARTPrefetcherState::WAITING_N_SCHEDULE_NEXT;
        }
        void setStateToReady() { state = ARTPrefetcherState::READY; }
        void setStateToWaiting() { state = ARTPrefetcherState::WAITING; }
        void setStateToInitial() { state = ARTPrefetcherState::INITIAL; }
        void setStateToEmpty() { state = ARTPrefetcherState::EMPTY; }
        void setStateToWaitingNScheduleNext() {
            state = ARTPrefetcherState::WAITING_N_SCHEDULE_NEXT;
        }
        void setStateToWaitingNCPUReserved() {
            state = ARTPrefetcherState::WAITING_N_CPU_RESERVED;
        }
        Addr setNextPrefetch(Addr addr) {
            nextPrefetchAddr = addr;
            nextTargetReady = true;
            return nextPrefetchAddr;
        }
        void allocateTarget(PacketPtr pkt, Tick when, Counter order) {
            target = Target(pkt, when, order);
            currentPrefetchAddr = pkt->getBlockAddr(blkSize);
            nextTargetReady = false;
        }
        bool hasNextPrefetch() const { return nextTargetReady; }
        Addr getNextPrefetchAddr() const { return nextPrefetchAddr; }
        bool isNextPrefetchAddr(Addr addr) const {
            return nextTargetReady && 
                              (nextPrefetchAddr == (addr & ~(blkSize - 1)));
        }
        Addr getCurrentPrefetchAddr() const { return currentPrefetchAddr; }

        // Printable
        void print(std::ostream &os, int verbosity = 0,
                   const std::string &prefix = "") const override;
        std::string print() const;

      private:
        // minimal address identity for the block
        Addr blkAddr = 0;
        unsigned blkSize = 8;
        bool isSecure = false;
        
        Addr currentPrefetchAddr = 0;
        Addr nextPrefetchAddr = 0;
        bool nextTargetReady = false;
        Tick allocTime = 0;
        ARTPrefetcherState state = ARTPrefetcherState::EMPTY;
        Target target;
    };

    ARTPrefetcherEntry artPrefetcher;
    ARTPrefetchBuffer artBuffer;

};
} // namespace gem5
#endif // __MEM_CACHE_ART_HH__