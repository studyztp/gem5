#include "art.hh"

namespace gem5
{

ART::ART(const ARTCacheParams& p)
  : Cache(p), 
    bypassCache(p.bypass_cache), 
    bypassPrefetch(p.bypass_prefetch),
    pfBlkSize(p.pf_blk_size),
    prefetchBuffer(pfBlkSize),
    currentBuffer(pfBlkSize)
{
    DPRINTF(ARTCache, 
        "ART Cache created with bypassCache=%s, bypassPrefetch=%s\n",
            bypassCache ? "true" : "false",
            bypassPrefetch ? "true" : "false");
    DPRINTF(ARTCache, 
        "ART Cache prefetch buffer size: %i bytes\n", pfBlkSize);
}

QueueEntry* ART::getNextQueueEntry() {
    return Cache::getNextQueueEntry();
}

bool ART::ifDataInCache(PacketPtr pkt) {
    // Check if the requested address is in the cache
    // Since this is meant to be a ready-only cache, we do not care if it is
    // dirty or not because we will never write to the memory.
    CacheBlk* blk = tags->findBlock({pkt->getAddr(), pkt->isSecure()});
    return blk && blk->isValid();
}

void ART::recvTimingReq(PacketPtr pkt) {
    // First, check if it is a read request because the ART cache is read-only
    assert(pkt->isRead()); 

    DPRINTF(ARTCache, "Received timing request for address: %s\n", 
            addrToString(pkt->getAddr()));
    DPRINTF(ARTCache, "Addr in cache: %s\n", 
                                            ifDataInCache(pkt) ? "Yes" : "No");

    // TODO: if bypassCache is true, we should forward the request to
    // memory directly. For now, we just ignore this flag.

    // If bypassPrefetch is true, we fall back to normal cache behavior
    // otherwise, we check the prefetch buffer first
    prefetch_hit = false;
    if (!bypassPrefetch) {
        // for now, let's assume the address request is always aligned with the
        // instruction line size, which is the prefetch block size
        if (currentBuffer.isHit(pkt->getAddr())) {
            DPRINTF(ARTCache, "Prefetch buffer hit for address: %s\n",
                    addrToString(pkt->getAddr()));
            // Serve data from prefetch buffer
            pkt->setDataFromBlock(currentBuffer.data.data(), currentBuffer.blk_size);
            pkt->makeTimingResponse();
            cpuSidePort.schedTimingResp(pkt, clockEdge(Cycles(1)));
            prefetch_hit = true;
        } else {
            DPRINTF(ARTCache, "Prefetch buffer miss for address: %s\n",
                    addrToString(pkt->getAddr()));
        }
        currentBuffer.copyFrom(prefetchBuffer);
        // transfer the data and addr from prefetchBuffer to currentBuffer
        prefetchBuffer.invalidate();
        // invalidate prefetchBuffer
        nextPfAddr = getNextSequentialAddr(pkt->getAddr());
        // calculate the next sequential address to prefetch
        DPRINTF(ARTCache, "Next sequential address to prefetch: %s\n",
                addrToString(nextPfAddr));
    }

    if(prefetch_hit) {
        return;
    }


    return Cache::recvTimingReq(pkt);
}

void ART::recvTimingResp(PacketPtr pkt) {
    // Continue with normal processing
    Cache::recvTimingResp(pkt);
}

} // namespace gem5