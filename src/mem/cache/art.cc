#include "art.hh"

namespace gem5
{

ART::ART(const ARTCacheParams& p)
  : NoncoherentCache(p), 
    bypassCache(p.bypass_cache), 
    bypassPrefetch(p.bypass_prefetch),
    pfBlkSize(p.pf_blk_size),
    prefetch_hit(false),
    artPrefetchOrder(0),
    ifPrefetchScheduled(false),
    scheduleForOverlappedPrefetch(false),
    prefetchBuffer(pfBlkSize),
    currentBuffer(pfBlkSize),
    artPfEntry("ART Prefetch Entry", pfBlkSize),
    bypassCacheEntry("Bypass Cache Entry")
{
    DPRINTF(ARTCache, 
        "ART Cache created with bypassCache=%s, bypassPrefetch=%s\n",
            bypassCache ? "true" : "false",
            bypassPrefetch ? "true" : "false");
    DPRINTF(ARTCache, 
        "ART Cache prefetch buffer size: %i bytes\n", pfBlkSize);
}

QueueEntry* ART::getNextQueueEntry() {
    if (bypassCacheEntry.ifRetrying() || 
                    (bypassCacheEntry.ready() && !artPfEntry.ifRetrying())) {
        // bypassCache has higher priority than prefetch entry if both are 
        // ready but if the prefetch entry is retrying, we should not starve it
        // by always picking the bypass entry
        DPRINTF(ARTCache, "ART getNextQueueEntry: returning bypass entry\n");
        return &bypassCacheEntry;
    }
    if (artPfEntry.ready()) {
        DPRINTF(ARTCache, "ART getNextQueueEntry: returning prefetch entry\n");
        return &artPfEntry;
    }
    return BaseCache::getNextQueueEntry();
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

    // If bypassPrefetch is true, we fall back to normal cache behavior
    // otherwise, we check the prefetch buffer first
    prefetch_hit = false;
    ifPrefetchScheduled = false;

    if (!bypassPrefetch && !pkt->isSecure()) {
        // for now, let's assume the address request is always aligned with the
        // instruction line size, which is the prefetch block size
        if (currentBuffer.isHit(pkt->getAddr())) {
            DPRINTF(ARTCache, "Current buffer hit for address: %s\n",
                    addrToString(pkt->getAddr()));
            // Serve data from prefetch buffer
            pkt->setDataFromBlock(
                currentBuffer.data.data(), currentBuffer.blk_size);
            pkt->makeTimingResponse();
            cpuSidePort.schedTimingResp(pkt, clockEdge(Cycles(1)));
            prefetch_hit = true;
        } else if (prefetchBuffer.isHit(pkt->getAddr())) {
            DPRINTF(ARTCache, "Prefetch buffer hit for address: %s\n",
                    addrToString(pkt->getAddr()));
            // Serve data from prefetch buffer
            pkt->setDataFromBlock(
                prefetchBuffer.data.data(), prefetchBuffer.blk_size);
            pkt->makeTimingResponse();
            cpuSidePort.schedTimingResp(pkt, clockEdge(Cycles(1)));
            prefetch_hit = true;
            prefetchBuffer.invalidate();
        }

        nextPfAddr = getNextSequentialAddr(pkt->getAddr());

        if (prefetchBuffer.isHit(nextPfAddr)) {
            DPRINTF(ARTCache, "Prefetch buffer hit for next prefetch address: %s\n",
                    addrToString(nextPfAddr));
            nextPfAddr = getNextSequentialAddr(nextPfAddr);
        } else if (artPfEntry.ifInService() && 
                        artPfEntry.matchBlockAddr(nextPfAddr, false)) {
            DPRINTF(ARTCache, "Next prefetch address matches in-service prefetch: %s\n",
                    addrToString(nextPfAddr));
            nextPfAddr = getNextSequentialAddr(nextPfAddr);
        }

        // calculate the next sequential address to prefetch
        DPRINTF(ARTCache, "Next sequential address to prefetch: %s\n",
                addrToString(nextPfAddr));
        
        // TODO: check if prefetch buffer hits

        // Hit or not, we need to clear the current buffer
        currentBuffer.invalidate();
        
        // Check if we can issue a prefetch request
        if (!artPfEntry.ifInService()) {
            if (prefetchBuffer.isValid()) {
                // If the prefetch buffer is valid, move it to current buffer
                currentBuffer.copyFrom(prefetchBuffer);
                DPRINTF(ARTCache, "Moved prefetch buffer to current buffer.\n");
                prefetchBuffer.invalidate();
            }
            if (artPfEntry.ifHasTarget()) {
                // If there is a target in the entry, we should make sure to
                // delete it first
                delete artPfEntry.getTarget()->pkt;
                artPfEntry.deallocate();
                artPfEntry.clearCPUWaiting();
            }
            // TODO: we assume that there is only one CPU here
            RequestPtr req = makeARTPrefetchRequest(nextPfAddr, pfBlkSize);
            // The packet is created inside makeARTPrefetchPacket()
            // The packet is deleted when the response is received
            PacketPtr pf_pkt = makeARTPrefetchPacket(req, &artPfEntry);
            assert(pf_pkt);
            artPfEntry.allocate(
                nextPfAddr, pf_pkt, clockEdge(Cycles(1)), artPrefetchOrder++);
            schedMemSideSendEvent(clockEdge());
            ifPrefetchScheduled = true;
            DPRINTF(ARTCache, "Scheduled prefetch for address: %s\n",
                    addrToString(nextPfAddr));
            assert(artPfEntry.ready());
        } else {
            DPRINTF(ARTCache, "Prefetch request already in service.\n");
            if (artPfEntry.matchBlockAddr(pkt)) {
                DPRINTF(ARTCache, "Request matches prefetch buffer.\n");
                if (ifDataInCache(pkt)) {
                    // Instead of waiting for the prefetch to complete,
                    // we can serve the request from cache if the data
                    // is already in cache
                    DPRINTF(ARTCache,
                            "Data already in cache, serving from cache.\n");
                } else {
                    assert(artPfEntry.setCPUWaiting(pkt));
                    prefetch_hit = true;
                    DPRINTF(ARTCache, "CPU request set to waiting state.\n");
                }
            }
        }
    }

    if(prefetch_hit) {
        return;
    } else {
        if (bypassCache) {
            DPRINTF(ARTCache, "Bypassing cache for address: %s\n",
                    addrToString(pkt->getAddr()));
            assert(bypassCacheEntry.ifTargetOpen());
            // Create a new target for the bypass entry
            bypassCacheEntry.allocate(
                pkt, clockEdge(Cycles(1)), artPrefetchOrder++);
            if (ifPrefetchScheduled) {
                // If there is prefetch scheduled above, we are overlapping
                // the prefetch with the bypass request, so we need to make
                // sure to schedule for the prefetch again after the
                // bypass request is sent
                scheduleForOverlappedPrefetch = true;
            } else {
                schedMemSideSendEvent(clockEdge());
            }
            return;
        }
    }

    return NoncoherentCache::recvTimingReq(pkt);
}

void ART::recvTimingResp(PacketPtr pkt) {
    if (bypassCacheEntry.ifInService()) {
        DPRINTF(ARTCache, "Received timing response for bypassed request.\n");
        BypassCacheEntry* entry =
            dynamic_cast<BypassCacheEntry*>(
                pkt->findNextSenderState<BypassCacheEntry>());
        if (entry) {
            assert(entry == &bypassCacheEntry);
            DPRINTF(ARTCache, "Received bypass response for address: %s\n",
                    addrToString(pkt->getAddr()));
            BypassCacheEntry* temp_entry = 
                        dynamic_cast<BypassCacheEntry*>(pkt->popSenderState());
            assert(temp_entry == entry);
            cpuSidePort.schedTimingResp(pkt, clockEdge(Cycles(1)));
            // Clean up the entry
            entry->deallocate();
            return;
        }
    }
    if (artPfEntry.ifInService()) {
        DPRINTF(ARTCache, "Received timing response while prefetch in service.\n");
        ARTPfQueueEntry* entry =
            dynamic_cast<ARTPfQueueEntry*>(
                pkt->findNextSenderState<ARTPfQueueEntry>());
        if (entry) {
            assert(entry == &artPfEntry);
            DPRINTF(ARTCache, "Received prefetch response for address: %s\n",
                    addrToString(entry->blkAddr));
            // Store the data in the prefetch buffer
            // assert(pkt->hasRespData());
            prefetchBuffer.storeData(
                entry->blkAddr, pkt->getPtr<uint8_t>()
            );
            DPRINTF(ARTCache, "Stored data in prefetch buffer for address: %s\n",
                    addrToString(entry->blkAddr));
            
            bool ifServedCPU = false;
            
            if (entry->ifCPUWaiting()) {
                // If there is a CPU request waiting, we need to serve it now
                // Copy the data to the cpu packet
                entry->cpuPtr->setDataFromBlock(
                    prefetchBuffer.data.data(), prefetchBuffer.blk_size);
                entry->cpuPtr->makeTimingResponse();
                cpuSidePort.schedTimingResp(
                    entry->cpuPtr, clockEdge(Cycles(1)));
                DPRINTF(ARTCache, "Served waiting CPU request for address: %s\n",
                        addrToString(entry->blkAddr));
                // Clear the waiting state
                entry->clearCPUWaiting();
                ifServedCPU = true;
                // Invalidate the current buffer as it has been used
            }

            // Clean up the entry
            delete pkt;
            entry->deallocate();

            if (!currentBuffer.isValid()) {
                // If the current buffer is open, move the prefetch buffer to
                // current buffer
                if (!ifServedCPU) {
                    // If the prefetch buffer already served a CPU request,
                    // we should not move it to current buffer
                    currentBuffer.copyFrom(prefetchBuffer);
                    DPRINTF(ARTCache, "Moved prefetch buffer to current buffer.\n");
                }

                prefetchBuffer.invalidate();

                if (entry->matchBlockAddr(nextPfAddr, false)) {
                    // If the next prefetch address is the same as the one
                    // just fetched, we need to update the next prefetch address
                    nextPfAddr = getNextSequentialAddr(nextPfAddr);
                    DPRINTF(ARTCache, "Updated next sequential address to prefetch: %s\n",
                            addrToString(nextPfAddr));
                }
                // Schedule the next prefetch if possible
                RequestPtr req = makeARTPrefetchRequest(nextPfAddr, pfBlkSize);
                PacketPtr pf_pkt = makeARTPrefetchPacket(req, &artPfEntry);
                assert(pf_pkt);
                artPfEntry.allocate(
                    nextPfAddr, pf_pkt, clockEdge(Cycles(1)), artPrefetchOrder++);
                schedMemSideSendEvent(clockEdge());
                DPRINTF(ARTCache, "Scheduled prefetch for address: %s\n",
                        addrToString(nextPfAddr));
            }
            return;
        }
    }

    // Continue with normal processing
    NoncoherentCache::recvTimingResp(pkt);
}

bool
ART::sendARTPrefetchPacket(ARTPfQueueEntry* entry) {
    assert (entry);
    assert (entry->ready());
    PacketPtr pkt = entry->getTarget()->pkt;
    assert (pkt);
    if (!memSidePort.sendTimingReq(pkt)) {
        DPRINTF(ARTCache, "Failed to send prefetch packet for address: %s\n",
                addrToString(entry->blkAddr));
        entry->markRetrying();
        // Failed to send the packet, return true then it will try again later
        return true;
    } else {
        entry->markInService();
        entry->clearRetrying();
        DPRINTF(ARTCache, "Sent prefetch packet for address: %s\n",
                addrToString(entry->blkAddr));
    }
    return false;
}

bool
ART::sendBypassPacket(BypassCacheEntry* entry) {
    assert (entry);
    PacketPtr pkt = entry->getTarget()->pkt;
    assert (pkt);
    if (!memSidePort.sendTimingReq(pkt)) {
        DPRINTF(ARTCache, "Failed to send bypass packet for address: %s\n",
                addrToString(pkt->getAddr()));
        // Failed to send the packet, return true then it will try again later
        entry->markRetrying();
        return true;
    } else {
        entry->markInService();
        entry->clearRetrying();
        DPRINTF(ARTCache, "Sent bypass packet for address: %s\n",
                addrToString(pkt->getAddr()));
        if (scheduleForOverlappedPrefetch) {
            // If the prefetch event was scheduled to overlap with a bypass
            // request, we need to make sure to schedule it again after the
            // bypass request is sent here
            DPRINTF(ARTCache, 
                        "Scheduling overlapped prefetch after bypass send.\n");
            schedMemSideSendEvent(clockEdge());
            scheduleForOverlappedPrefetch = false;
        }
    }
    return false;
}

} // namespace gem5