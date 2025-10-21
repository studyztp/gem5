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
    if (bypassCache) {
        // bypassCache target always has higher priority than Prefetch target
        // because bypassCache target is for certain that it is needed by CPU.
        // However, if the prefetch target is already retrying, we should not
        // interrupt it with a new bypassCache target.
        if (bypassCacheEntry.ifRetrying() ||
                    (bypassCacheEntry.ready() && !artPfEntry.ifRetrying())) {

            DPRINTF(ARTCache,
                    "ART::getNextQueueEntry: returning BypassCache entry\n");
            return &bypassCacheEntry;
        }
    } else {
        // BaseCache always having the higher priority because if there is a
        // cache miss or access, it is more urgent than prefetch.
        QueueEntry* baseEntry = BaseCache::getNextQueueEntry();
        if (baseEntry) {
            DPRINTF(ARTCache,
                "ART::getNextQueueEntry: returning BaseCache entry\n");
            return baseEntry;
        }
    }

    if (artPfEntry.ready()) {
        DPRINTF(ARTCache, "ART::getNextQueueEntry: returning Prefetch "
                                                                    "entry\n");
        return &artPfEntry;
    }

    DPRINTF(ARTCache, "ART::getNextQueueEntry: no entry ready\n");
    return nullptr;
}

Tick ART::nextQueueReadyTime() const {
    Tick next_time = BaseCache::nextQueueReadyTime();
    if (bypassCacheEntry.ready() && next_time == MaxTick) {
        next_time = curTick();
        DPRINTF(ARTCache, "ART::nextQueueReadyTime:[BypassCache] "
                                                "BypassCache entry ready\n");
    }
    if (artPfEntry.ready() && next_time == MaxTick) {
        next_time = curTick();
        DPRINTF(ARTCache, "ART::nextQueueReadyTime:[Prefetch] "
                                                "Prefetch entry ready\n");
    }
    return next_time;
}

bool ART::ifDataInCache(PacketPtr pkt) {
    // Since this is meant to be a ready-only cache, we do not care if it is
    // dirty or not because we will never write to the memory.
    CacheBlk* blk = tags->findBlock({pkt->getAddr(), pkt->isSecure()});
    return blk && blk->isValid();
}

void ART::recvTimingReq(PacketPtr pkt) {
    // First, check if it is a read request because the ART cache is read-only.
    panic_if(!pkt->isRead(), "ART Cache only supports read requests.\n");

    // TODO: we assume there is only one CPU and one hierarchy cache, which is
    // the cache for ART accelerator in STM32-G4.
    DPRINTF(ARTCache, "ART::recvTimingReq: request packet address: %s\n",
                                                addrToString(pkt->getAddr()));

    // Check if the request can be served from current buffer or the prefetch
    // buffer (including currently prefetching line when data not in ICache).
    prefetch_hit = false;

    // Only handle prefetch if it is enabled.
    // TODO: we don't deal with secure requests for now.
    if (!bypassPrefetch && !pkt->isSecure()) {
        // Check if the current buffer has the request address
        if (currentBuffer.isHit(pkt->getAddr())) {
            DPRINTF(ARTCache,
                "ART::recvTimingReq:[Prefetch]: Current buffer hits \n");
            // Serve data from current buffer
            pkt->setDataFromBlock(
                currentBuffer.data.data(), currentBuffer.blk_size);
            pkt->makeTimingResponse();
            DPRINTF(ARTCache,
                "ART::recvTimingReq:[Prefetch]: Schedule CPU port response\n");
            cpuSidePort.schedTimingResp(pkt, clockEdge(Cycles(1)));
            prefetch_hit = true;
        // Check if the prefetch buffer has the request address.
        // Note that the prefetch buffer can only return isHit() true if it has
        // valid data.
        } else if (prefetchBuffer.isHit(pkt->getAddr())) {
            DPRINTF(ARTCache,
                "ART::recvTimingReq:[Prefetch]: Prefetch buffer hits \n");
            // Serve data from prefetch buffer
            pkt->setDataFromBlock(
                prefetchBuffer.data.data(), prefetchBuffer.blk_size);
            pkt->makeTimingResponse();
            DPRINTF(ARTCache,
                "ART::recvTimingReq:[Prefetch]: Schedule CPU port response\n");
            cpuSidePort.schedTimingResp(pkt, clockEdge(Cycles(1)));
            prefetch_hit = true;
            DPRINTF(ARTCache,
                "ART::recvTimingReq:[Prefetch]: Invalidate prefetch buffer\n");
            prefetchBuffer.invalidate();
        }

        // Calculate the next sequential address as our default next prefetch
        // address
        nextPfAddr = getNextSequentialAddr(pkt->getAddr());
        DPRINTF(ARTCache,
            "ART::recvTimingReq:[Prefetch]: Next sequential address of the "
                "request packet's address: %s\n", addrToString(nextPfAddr));

        // If the next prefetch address is already in the prefetch buffer, we
        // should skip it and move to the next of that.
        if (prefetchBuffer.isHit(nextPfAddr)) {
            DPRINTF(ARTCache,
                "ART::recvTimingReq:[Prefetch]: Next sequential address hits "
                            "prefetch buffer.\n");
            nextPfAddr = getNextSequentialAddr(nextPfAddr);
        } else if (artPfEntry.ifInService() &&
                        artPfEntry.matchBlockAddr(nextPfAddr, false)) {
            DPRINTF(ARTCache,
                "ART::recvTimingReq:[Prefetch]: Next sequential address hits "
                            "the prefetch in service.\n");
            nextPfAddr = getNextSequentialAddr(nextPfAddr);
        }

        // In here, we have the true next prefetch address to use.
        DPRINTF(ARTCache,
            "ART::recvTimingReq:[Prefetch]: Final next prefetch address: %s\n",
                addrToString(nextPfAddr));

        // Invalidate the current buffer because it is been accessed.
        DPRINTF(ARTCache,
            "ART::recvTimingReq:[Prefetch]: Invalidated current buffer.\n");
        currentBuffer.invalidate();


        // Check if the prefetch entry is in service. If not, we can setup the
        // next prefetch request.
        // If yes, there are two cases:
        // 1) the request packet can be served by the current buffer or the
        // cache. In this case, we don't do anything.
        // 2) the request packet cannot be served by any of the above. In this
        // case, we need to check if the request packet matches the address the
        // prefetch entry is waiting for. a) If yes, we set the CPU waiting
        // flag in the prefetch entry so that when the prefetch response comes,
        // we can serve the CPU request immediately. b) If not, we don't do
        // anything and let it go to the normal cache access flow.
        if (!artPfEntry.ifInService()) {
            // If the prefetch buffer is valid (has data and not yet used),
            // we should move it to current buffer before we issue a new
            // prefetch request.
            DPRINTF(ARTCache,
                "ART::recvTimingReq:[Prefetch]: prefetch NOT in service.\n");
            if (prefetchBuffer.isValid()) {
                DPRINTF(ARTCache,
                    "ART::recvTimingReq:[Prefetch]: Move prefetch buffer to "
                    "current buffer and invalidate the prefetch buffer.\n");
                currentBuffer.copyFrom(prefetchBuffer);
                prefetchBuffer.invalidate();
            }
            // If the prefetch entry has a target, it means that the previous
            // prefetch request is not yet in service (i.e., waiting for retry)
            // so we need to delete the previous target first.
            // This is possible even for single issue single width CPU because
            // the prefetch request is setup speculatively.
            if (artPfEntry.ifHasTarget()) {
                DPRINTF(ARTCache,
                    "ART::recvTimingReq:[Prefetch]: Previous prefetch target "
                    "with address %s is not yet in service, deleting it.\n",
                    addrToString(artPfEntry.getTarget()->pkt->getAddr()));
                delete artPfEntry.getTarget()->pkt;
                artPfEntry.deallocate();
                artPfEntry.clearCPUWaiting();
                // Make sure the retrying flag is cleared as well so it doesn't
                // block the priority of bypassCacheEntry.
                artPfEntry.clearRetrying();
            }
            // The prefetch entry has its own request and packet creation.
            // When the packet finished its service, it will copy the data
            // to the prefetch buffer and then we handle the delete there.
            // The request should be handled by C++ smart pointer
            // (std::make_shared<Request>).
            RequestPtr req = makeARTPrefetchRequest(nextPfAddr, pfBlkSize);
            PacketPtr pf_pkt = makeARTPrefetchPacket(req, &artPfEntry);
            panic_if(!pf_pkt, "Failed to create prefetch packet in "
                                                    "ART::recvTimingReq().\n");
            // The prefetch entry will store the packet into itself using the
            // Target class method.
            // Using this method is easier for us to leverage the inherited
            // scheduling and retrying mechanism from the BaseCache class.
            // Therefore, the Target object doesn't do much but holding the
            // packet and its state.
            DPRINTF(ARTCache,
                "ART::recvTimingReq:[Prefetch]: Allocate Target for prefetch "
                                                                "entry.\n");
            artPfEntry.allocate(
                nextPfAddr, pf_pkt, clockEdge(Cycles(1)), artPrefetchOrder++);
            if (prefetch_hit) {
                DPRINTF(ARTCache,
                    "ART::recvTimingReq:[Prefetch]: Schedule prefetch request "
                        "to memory side port for address %s at %llu\n",
                                        addrToString(nextPfAddr), clockEdge());
                schedMemSideSendEvent(clockEdge());
            } else {
                DPRINTF(ARTCache,
                    "ART::recvTimingReq:[Prefetch]: Prefetch request will be "
                    "scheduled after cache event.\n"
                );
            }
            panic_if(!artPfEntry.ready(), "Prefetch entry is not ready after "
                                    "allocation in ART::recvTimingReq().\n");
        } else {
            DPRINTF(ARTCache,
                "ART::recvTimingReq:[Prefetch]: prefetch in service.\n");
            if (artPfEntry.matchBlockAddr(pkt)) {
                DPRINTF(ARTCache,
                    "ART::recvTimingReq:[Prefetch]: Request address matches "
                    "the prefetch in service.\n");
                if (ifDataInCache(pkt)) {
                    // Instead of waiting for the prefetch to complete,
                    // we can serve the request from cache if the data
                    // is already in cache
                    DPRINTF(ARTCache,
                        "ART::recvTimingReq:[Prefetch]: Data is already in "
                        "cache, serving from cache.\n");
                } else {
                    assert(artPfEntry.setCPUWaiting(pkt));
                    panic_if(!artPfEntry.ifCPUWaiting(),
                        "Failed to set CPU waiting flag in "
                                                    "ART::recvTimingReq().\n");
                    prefetch_hit = true;
                    DPRINTF(ARTCache,
                        "ART::recvTimingReq:[Prefetch]: Set CPU waiting flag "
                        "in prefetch entry.\n");
                }
            }
        }
    }

    if (prefetch_hit)
    {
        DPRINTF(ARTCache,
            "ART::recvTimingReq: Request served by prefetch mechanism so "
                                                "skipping cache access.\n");
        return;
    } else {
        // Since ART allows us to bypass the ICache, we should also model it.
        // If the request address is not in cache, we can bypass the cache
        // and send it directly to memory.
        // This is handle by the bypassCacheEntry which is a single entry
        // queue similar to the prefetch entry but has two Target objects:
        // one for the current request and one for the next request.
        if (bypassCache) {
            DPRINTF(ARTCache,
                "ART::recvTimingReq:[BypassCache]: "
                    "Bypass cache is enabled and bypassing %s.\n",
                                                addrToString(pkt->getAddr()));
            panic_if(!bypassCacheEntry.ifTargetOpen(),
                "Bypass cache entry bypass target is not empty in "
                                                    "ART::recvTimingReq().\n");
            // Create a new target for the bypass entry
            DPRINTF(ARTCache,
                "ART::recvTimingReq:[BypassCache]: Allocate Target for bypass "
                                                "cache entry.\n");
            bypassCacheEntry.allocate(
                pkt, clockEdge(Cycles(1)), artPrefetchOrder++);
            panic_if(!bypassCacheEntry.ready(),
                "Bypass cache entry is not ready after allocation in "
                                                    "ART::recvTimingReq().\n");

            DPRINTF(ARTCache,
                "ART::recvTimingReq:[BypassCache]: Schedule bypass cache "
                "request to memory side port for address %s at %llu\n",
                                addrToString(pkt->getAddr()), clockEdge());
            schedMemSideSendEvent(clockEdge());

            return;
        }
    }

    DPRINTF(ARTCache,
        "ART::recvTimingReq: Passing request to "
                                        "NoncoherentCache::recvTimingReq()\n");
    // Continue with normal processing
    return NoncoherentCache::recvTimingReq(pkt);
}

void ART::recvTimingResp(PacketPtr pkt) {
    // Response only happen for sent requests, which we should only check the
    // entries in service.
    if (bypassCacheEntry.ifInService()) {
        DPRINTF(ARTCache,
            "ART::recvTimingResp:[BypassCache]: Received timing response "
                                            "while bypass is in service.\n");
        BypassCacheEntry* entry =
            dynamic_cast<BypassCacheEntry*>(
                pkt->findNextSenderState<BypassCacheEntry>());
        if (entry) {
            panic_if(entry != &bypassCacheEntry,
                "Bypass cache entry mismatch in ART::recvTimingResp().\n");
            DPRINTF(ARTCache,
                "ART::recvTimingResp:[BypassCache]: Received response for "
                "address %s\n", addrToString(entry->blkAddr));
            BypassCacheEntry* temp_entry =
                        dynamic_cast<BypassCacheEntry*>(pkt->popSenderState());
            panic_if(temp_entry != entry,
                "Bypass cache entry pop mismatch in ART::recvTimingResp().\n");
            DPRINTF(ARTCache,
                "ART::recvTimingResp:[BypassCache]: Schedule CPU port response"
                " for the bypassed request at %llu.\n", clockEdge(Cycles(1)));
            cpuSidePort.schedTimingResp(pkt, clockEdge(Cycles(1)));
            // Clean up the entry
            DPRINTF(ARTCache,
                "ART::recvTimingResp:[BypassCache]: Deallocate bypass cache "
                                                                "entry.\n");
            entry->deallocate();
            return;
        }
    }
    if (artPfEntry.ifInService()) {
        DPRINTF(ARTCache,
            "ART::recvTimingResp:[Prefetch]: Received timing response while "
                                                    "prefetch in service.\n");
        ARTPfQueueEntry* entry =
            dynamic_cast<ARTPfQueueEntry*>(
                pkt->findNextSenderState<ARTPfQueueEntry>());
        if (entry) {
            panic_if(entry != &artPfEntry,
                "Prefetch entry mismatch in ART::recvTimingResp().\n");
            DPRINTF(ARTCache,
                "ART::recvTimingResp:[Prefetch]: Received response for "
                "address %s. Now store data in prefetch buffer.\n",
                                                addrToString(entry->blkAddr));
            // Store the data in the prefetch buffer
            prefetchBuffer.storeData(
                entry->blkAddr, pkt->getPtr<uint8_t>()
            );

            bool ifServedCPU = false;

            // If the data is needed by CPU, we need to serve it now
            if (entry->ifCPUWaiting()) {
                DPRINTF(ARTCache,
                    "ART::recvTimingResp:[Prefetch]: Prefetch entry has CPU "
                    "waiting, serve the CPU now.\n");
                entry->cpuPtr->setDataFromBlock(
                    prefetchBuffer.data.data(), prefetchBuffer.blk_size);
                entry->cpuPtr->makeTimingResponse();
                DPRINTF(ARTCache,
                    "ART::recvTimingResp:[Prefetch]: Schedule CPU port "
                    "response for the waiting CPU request at %llu.\n",
                                                clockEdge(Cycles(1)));
                cpuSidePort.schedTimingResp(
                    entry->cpuPtr, clockEdge(Cycles(1)));
                // Clear the waiting state
                entry->clearCPUWaiting();
                ifServedCPU = true;
            }

            // Clean up the entry
            DPRINTF(ARTCache, "ART::recvTimingResp:[Prefetch]: Deallocate "
                                    "prefetch entry and delete the packet.\n");
            delete pkt;
            entry->deallocate();

            // Only prefetch a new line if we have buffer space.
            // If the current buffer is valid, it means that both the current
            // buffer and the prefetch buffer are occupied, we should not
            // issue a new prefetch.
            if (!currentBuffer.isValid()) {
                // If the prefetch buffer just served a CPU request, we should
                // just abandon it for new prefetch.
                // Also, logically, if the prefetch buffer has a CPU waiting,
                // then the current buffer must be invalidated.
                if (!ifServedCPU) {
                    currentBuffer.copyFrom(prefetchBuffer);
                    DPRINTF(ARTCache, "ART::recvTimingResp:[Prefetch]: "
                                "Move prefetch buffer to current buffer.\n");
                }

                DPRINTF(ARTCache, "ART::recvTimingResp:[Prefetch]: "
                            "Invalidate prefetch buffer for new prefetch.\n");
                prefetchBuffer.invalidate();

                // If the next prefetch address set in recvTimingReq() is the
                // same as the one just fetched, we need to update it.
                // This is already guarded in recvTimingReq() but just in case
                // something changed in between.
                if (entry->matchBlockAddr(nextPfAddr, false)) {
                    nextPfAddr = getNextSequentialAddr(nextPfAddr);
                    DPRINTF(ARTCache,
                        "ART::recvTimingResp:[Prefetch]: Next prefetch address"
                        " after response matches the fetched address, update "
                        "to the next sequential address: %s\n",
                                                    addrToString(nextPfAddr));
                }
                // Create a new prefetch request and packet
                DPRINTF(ARTCache,
                    "ART::recvTimingResp:[Prefetch]: Allocate new Target in "
                                "prefetch entry for new prefetch target.\n");
                RequestPtr req = makeARTPrefetchRequest(nextPfAddr, pfBlkSize);
                PacketPtr pf_pkt = makeARTPrefetchPacket(req, &artPfEntry);
                panic_if(!pf_pkt, "Failed to create prefetch packet in "
                                                "ART::recvTimingResp().\n");
                artPfEntry.allocate(nextPfAddr, pf_pkt, clockEdge(Cycles(1)),
                                                        artPrefetchOrder++);
                DPRINTF(ARTCache,
                    "ART::recvTimingResp:[Prefetch]: Schedule prefetch request"
                    " to memory side port for address %s at %llu\n",
                                        addrToString(nextPfAddr), clockEdge());
                schedMemSideSendEvent(clockEdge());
            }
            return;
        }
    }

    DPRINTF(ARTCache,
        "ART::recvTimingResp: Passing response to "
                                    "NoncoherentCache::recvTimingResp()\n");
    // Continue with normal processing
    NoncoherentCache::recvTimingResp(pkt);
}

bool
ART::sendARTPrefetchPacket(ARTPfQueueEntry* entry) {
    panic_if(!entry, "Null prefetch entry in ART::sendARTPrefetchPacket().\n");
    panic_if(!entry->ready(),
        "Prefetch entry not ready in ART::sendARTPrefetchPacket().\n");
    PacketPtr pkt = entry->getTarget()->pkt;
    panic_if(!pkt, "Null prefetch packet in ART::sendARTPrefetchPacket().\n");
    if (!memSidePort.sendTimingReq(pkt)) {
        DPRINTF(ARTCache,
                "ART::sendARTPrefetchPacket:[Prefetch] Failed to send prefetch"
                " packet for address: %s\n", addrToString(entry->blkAddr));
        entry->markRetrying();
        // Failed to send the packet, return true then it will try again later
        return true;
    } else {
        entry->markInService();
        entry->clearRetrying();
        DPRINTF(ARTCache,
            "ART::sendARTPrefetchPacket:[Prefetch] Sent prefetch packet for "
                                "address: %s\n", addrToString(entry->blkAddr));
    }
    return false;
}

bool
ART::sendBypassPacket(BypassCacheEntry* entry) {
    panic_if(!entry, "Null bypass cache entry in ART::sendBypassPacket().\n");
    panic_if(!entry->ready(),
        "Bypass cache entry not ready in ART::sendBypassPacket().\n");
    PacketPtr pkt = entry->getTarget()->pkt;
    panic_if(!pkt, "Null bypass packet in ART::sendBypassPacket().\n");
    if (!memSidePort.sendTimingReq(pkt)) {
        DPRINTF(ARTCache, "ART::sendBypassPacket:[Bypass] Failed to send "
            "bypass packet for address: %s\n", addrToString(pkt->getAddr()));
        // Failed to send the packet, return true then it will try again later
        entry->markRetrying();
        return true;
    } else {
        entry->markInService();
        entry->clearRetrying();
        DPRINTF(ARTCache, "ART::sendBypassPacket:[Bypass] Sent bypass packet "
                            "for address: %s\n", addrToString(pkt->getAddr()));
    }
    return false;
}

} // namespace gem5
