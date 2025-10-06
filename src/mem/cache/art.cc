#include "art.hh"

ART::ART(const CacheParams& p)
  : Cache(p),
    artPrefetcher("ART Prefetcher Entry"),
    artBuffer(/*blk_size=*/8) {}

QueueEntry* ART::getNextQueueEntry() {
    // If the prefetcher has an initialized and ready prefetch, return it
    if (artPrefetcher.isInitial()) {
        return &artPrefetcher;
    }

    return Cache::getNextQueueEntry();
}

bool ART::ifDataInCache(PacketPtr pkt) {
    // Check if the requested address is in the cache
    // Since this is meant to be a ready-only cache, we do not care if it is
    // dirty or not because we will never write to the memory.
    const bool sec = pkt->isSecure();
    CacheBlk* blk = tags->findBlock(pkt->getAddr(), sec);
    return blk && blk->isValid();
}

bool ART::recvTimingReq(PacketPtr pkt) {
    // First, check if it is a read request because the ART cache is read-only
    assert(pkt->isRead()); 

    bool prefetch_hit = false;

    if (artBuffer.valid && artBuffer.matches(pkt->getAddr())) {
        DPRINTF(
            Cache, "%s: Satisfying request %s from ART Prefetched Buffer\n",
            __func__, pkt->print()
        );
        artBuffer.copyData(pkt);
        pkt->makeTimingResponse();  // Changes packet from request to response
        // Schedule the response to be sent back to the CPU after lookup 
        // latency
        Cycles lat = lookupLatency;  // Fast ART buffer access
        Tick response_time = clockEdge(lat);
        cpuSidePort.schedTimingResp(pkt, response_time);
        prefetch_hit = true;
    }

    // If the prefetcher is not waiting for a request to complete, set up the 
    // next prefetch

    // The prefetcher prefetched to the prefetch buffer but has not
    // setup the next prefetch yet. Do it now.
    Addr next_addr = artPrefetcher.setNextPrefetch(
        artBuffer.getNextAddr(pkt->getAddr())
    );
    DPRINTF(
        Cache, "%s: Setting up next prefetch to address 0x%x\n",
        __func__, next_addr
    );

    if (!(artPrefetcher.isWaiting() || 
          artPrefetcher.isWaitingNCPUReserved() ||
          artPrefetcher.isWaitingNScheduleNext())) {
        if (prefetch_hit) {
            artPrefetcher.setStateToWaitingNScheduleNext();
        } else {
            if(!ifDataInCache(pkt) && 
                artPrefetcher.getCurrentPrefetchAddr()==(
                    pkt->getBlockAddr(artBuffer.size))) {
                // If the requested address is not in the cache, and it is the
                // same as the current prefetch address, we need to wait for
                // the CPU to reserve a line before we can prefetch again.
                artPrefetcher.setStateToWaitingNCPUReserved();
                prefetch_hit = true; // To avoid sending the request to cache
            } else {
                artPrefetcher.setStateToWaitingNScheduleNext();
            }
        }
    } else {
        artPrefetcher.setStateToInitial();
        schedMemSideSendEvent(clockEdge(Cycles(1)));
    }

    if (prefetch_hit) {
        // If we satisfied the request from the prefetch buffer, we are done
        return true;
    }
    
    // Continue with normal processing
    return Cache::recvTimingReq(pkt);
}
