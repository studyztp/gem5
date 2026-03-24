/**
 * @file
 * Implementation of the ART (Adaptive Real-Time) accelerator cache.
 */

#include "mem/cache/art.hh"

namespace gem5
{

ART::ART(const ARTCacheParams &p)
    : NoncoherentCache(p),
      directMemoryMode(p.direct_memory_mode),
      enablePrefetch(p.enable_prefetch),
      prefetchOnCacheHit(p.prefetch_on_cache_hit),
      pfBlkSize(p.pf_blk_size),
      artPrefetchOrder(0),
      artRequestorId(p.system->getRequestorId(this)),
      flashStartAddr(p.flash_start_addr),
      flashEndAddr(p.flash_end_addr),
      prefetchBuffer(pfBlkSize),
      currentBuffer(pfBlkSize),
      artPfEntry("ART Prefetch Entry", pfBlkSize),
      bypassCacheEntry("Bypass Cache Entry"),
      stats(*this)
{
    fatal_if(!isPowerOf2(pfBlkSize),
             "ART prefetch block size must be a power of 2, got %u",
             pfBlkSize);
    fatal_if(flashStartAddr > flashEndAddr,
             "ART flash start address (%#x) must be <= end address (%#x)",
             flashStartAddr, flashEndAddr);

    DPRINTF(ARTCache,
            "ART created: directMemoryMode=%s, enablePrefetch=%s, "
            "prefetchOnCacheHit=%s, pfBlkSize=%u, flash=[%#x, %#x]\n",
            directMemoryMode ? "true" : "false",
            enablePrefetch ? "true" : "false",
            prefetchOnCacheHit ? "true" : "false", pfBlkSize, flashStartAddr,
            flashEndAddr);
}

// -------------------------------------------------------------------
//  Statistics
// -------------------------------------------------------------------

ART::ARTStats::ARTStats(ART &art)
    : statistics::Group(&art),
      ADD_STAT(currentBufferHits, statistics::units::Count::get(),
               "Requests served from the current buffer"),
      ADD_STAT(prefetchBufferHits, statistics::units::Count::get(),
               "Requests served from the prefetch buffer"),
      ADD_STAT(prefetchMisses, statistics::units::Count::get(),
               "Requests not served by either prefetch buffer"),
      ADD_STAT(bypassAccesses, statistics::units::Count::get(),
               "Requests forwarded directly to memory (bypass mode)"),
      ADD_STAT(cpuWaitEvents, statistics::units::Count::get(),
               "Times the CPU had to wait for an in-flight prefetch"),
      ADD_STAT(prefetchesAllocated, statistics::units::Count::get(),
               "Total prefetch entries allocated (includes discarded)"),
      ADD_STAT(prefetchesIssued, statistics::units::Count::get(),
               "Total prefetch requests actually sent to memory"),
      ADD_STAT(prefetchesCompleted, statistics::units::Count::get(),
               "Total prefetch responses received from memory")
{
}

// -------------------------------------------------------------------
//  Queue scheduling
// -------------------------------------------------------------------

QueueEntry *
ART::getNextQueueEntry()
{
    if (directMemoryMode) {
        /*
         * In direct-memory mode the bypass entry has higher priority
         * than a prefetch, because bypass traffic is demand traffic
         * that the CPU is actively waiting on.  However, if the
         * prefetch entry is already retrying we must not preempt it.
         */
        if (bypassCacheEntry.ifRetrying() ||
            (bypassCacheEntry.ready() && !artPfEntry.ifRetrying())) {
            DPRINTF(ARTCache,
                    "getNextQueueEntry: returning BypassCache entry\n");
            return &bypassCacheEntry;
        }
    } else {
        /*
         * When the cache is active, base-cache entries (MSHR / WB)
         * take priority over prefetch because they represent demand
         * traffic.
         */
        QueueEntry *baseEntry = BaseCache::getNextQueueEntry();
        if (baseEntry) {
            DPRINTF(ARTCache,
                    "getNextQueueEntry: returning BaseCache entry\n");
            return baseEntry;
        }
    }

    if (artPfEntry.ready()) {
        DPRINTF(ARTCache,
                "getNextQueueEntry: returning Prefetch entry\n");
        return &artPfEntry;
    }

    DPRINTF(ARTCache, "getNextQueueEntry: no entry ready\n");
    return nullptr;
}

Tick
ART::nextQueueReadyTime() const
{
    Tick nextTime = BaseCache::nextQueueReadyTime();

    if (bypassCacheEntry.ready() && nextTime == MaxTick) {
        nextTime = curTick();
        DPRINTF(ARTCache,
                "nextQueueReadyTime: BypassCache entry ready\n");
    }
    if (artPfEntry.ready() && nextTime == MaxTick) {
        nextTime = curTick();
        DPRINTF(ARTCache,
                "nextQueueReadyTime: Prefetch entry ready\n");
    }
    return nextTime;
}

// -------------------------------------------------------------------
//  Helpers
// -------------------------------------------------------------------

bool
ART::isDataInCache(PacketPtr pkt)
{
    CacheBlk *blk = tags->findBlock({pkt->getAddr(), pkt->isSecure()});
    return blk && blk->isValid();
}

void
ART::serveFromBuffer(PacketPtr pkt, ARTPrefetchBuffer &buf)
{
    pkt->setDataFromBlock(buf.data.data(), buf.blkSize);
    pkt->makeTimingResponse();
    cpuSidePort.schedTimingResp(pkt, clockEdge(Cycles(1)));
}

// -------------------------------------------------------------------
//  Timing request path
// -------------------------------------------------------------------

void
ART::recvTimingReq(PacketPtr pkt)
{
    panic_if(!pkt->isRead(), "ART only supports read requests.");

    DPRINTF(ARTCache, "recvTimingReq: addr=%s\n",
            addrToString(pkt->getAddr()));

    {
        std::string curBufStr =
            currentBuffer.isValid()
                ? "valid addr=" + addrToString(currentBuffer.addr)
                : "invalid";
        std::string pfBufStr =
            prefetchBuffer.isValid()
                ? "valid addr=" + addrToString(prefetchBuffer.addr)
                : "invalid";
        std::string pfEntryStr =
            artPfEntry.ifInService()
                ? "IN_SVC@" + addrToString(artPfEntry.blkAddr)
            : artPfEntry.ifHasTarget()
                ? "ALLOC@" + addrToString(artPfEntry.blkAddr)
                : "EMPTY";
        DPRINTF(ARTCache,
                "recvTimingReq: state curBuf=(%s) pfBuf=(%s) "
                "pfEntry=%s\n",
                curBufStr, pfBufStr, pfEntryStr);
    }

    bool prefetchHit = false;

    if (enablePrefetch && !pkt->isSecure()) {
        // --- Try to serve from the current buffer ---
        if (currentBuffer.isHit(pkt->getAddr())) {
            DPRINTF(ARTCache, "recvTimingReq: current buffer hit\n");
            serveFromBuffer(pkt, currentBuffer);
            prefetchHit = true;
            ++stats.currentBufferHits;
            DPRINTF(ARTCache,
                    "recvTimingReq: stats hits=%d pfHits=%d "
                    "misses=%d alloc=%d issued=%d completed=%d\n",
                    stats.currentBufferHits.value(),
                    stats.prefetchBufferHits.value(),
                    stats.prefetchMisses.value(),
                    stats.prefetchesAllocated.value(),
                    stats.prefetchesIssued.value(),
                    stats.prefetchesCompleted.value());

        } else if (prefetchBuffer.isHit(pkt->getAddr())) {
            DPRINTF(ARTCache, "recvTimingReq: prefetch buffer hit\n");
            serveFromBuffer(pkt, prefetchBuffer);
            prefetchHit = true;
            ++stats.prefetchBufferHits;
            DPRINTF(ARTCache,
                    "recvTimingReq: stats hits=%d pfHits=%d "
                    "misses=%d alloc=%d issued=%d completed=%d\n",
                    stats.currentBufferHits.value(),
                    stats.prefetchBufferHits.value(),
                    stats.prefetchMisses.value(),
                    stats.prefetchesAllocated.value(),
                    stats.prefetchesIssued.value(),
                    stats.prefetchesCompleted.value());
            prefetchBuffer.invalidate();
        }

        // Compute the candidate next-prefetch address.
        nextPfAddr = getNextSequentialAddr(pkt->getAddr());
        DPRINTF(ARTCache, "recvTimingReq: next sequential addr=%s\n",
                addrToString(nextPfAddr));

        // Skip ahead if that address is already covered.
        if (prefetchBuffer.isHit(nextPfAddr)) {
            DPRINTF(ARTCache,
                    "recvTimingReq: next addr already in prefetch buffer, "
                    "advancing\n");
            nextPfAddr = getNextSequentialAddr(nextPfAddr);
        } else if (artPfEntry.ifInService() &&
                   artPfEntry.matchBlockAddr(nextPfAddr, false)) {
            DPRINTF(ARTCache,
                    "recvTimingReq: next addr already in-flight, "
                    "advancing\n");
            nextPfAddr = getNextSequentialAddr(nextPfAddr);
        }

        DPRINTF(ARTCache, "recvTimingReq: final next prefetch addr=%s\n",
                addrToString(nextPfAddr));

        currentBuffer.invalidate();

        if (!artPfEntry.ifInService()) {
            // Promote prefetchBuffer -> currentBuffer if possible.
            if (prefetchBuffer.isValid()) {
                DPRINTF(ARTCache,
                        "recvTimingReq: promoting prefetch -> current "
                        "buffer\n");
                currentBuffer.copyFrom(prefetchBuffer);
                prefetchBuffer.invalidate();
            }

            /*
             * If a previous prefetch target is still waiting for
             * retry, discard it so we can issue the new one.
             */
            if (artPfEntry.ifHasTarget()) {
                DPRINTF(ARTCache,
                        "recvTimingReq: discarding stale prefetch "
                        "target addr=%s\n",
                        addrToString(
                            artPfEntry.getTarget()->pkt->getAddr()));
                delete artPfEntry.getTarget()->pkt;
                artPfEntry.deallocate();
                artPfEntry.clearCPUWaiting();
                artPfEntry.clearRetrying();
            }

            // Only issue the prefetch if it falls within flash range.
            if (isInFlashRange(nextPfAddr)) {
                RequestPtr req =
                    makeARTPrefetchRequest(nextPfAddr, pfBlkSize);
                PacketPtr pfPkt =
                    makeARTPrefetchPacket(req, &artPfEntry);
                panic_if(!pfPkt,
                         "Failed to create prefetch packet in "
                         "recvTimingReq.");

                artPfEntry.allocate(nextPfAddr, pfPkt,
                                    clockEdge(Cycles(1)),
                                    artPrefetchOrder++);
                ++stats.prefetchesAllocated;
                DPRINTF(ARTCache,
                        "recvTimingReq: allocated prefetch "
                        "for addr=%s (allocated=%d)\n",
                        addrToString(nextPfAddr),
                        stats.prefetchesAllocated.value());

                if (prefetchHit) {
                    DPRINTF(ARTCache,
                            "recvTimingReq: scheduling prefetch for "
                            "%s immediately\n",
                            addrToString(nextPfAddr));
                    schedMemSideSendEvent(clockEdge());
                } else {
                    DPRINTF(ARTCache,
                            "recvTimingReq: prefetch will be "
                            "scheduled after cache event\n");
                }

                panic_if(!artPfEntry.ready(),
                         "Prefetch entry not ready after allocation.");
            } else {
                DPRINTF(ARTCache,
                        "recvTimingReq: next prefetch addr=%s outside "
                        "flash range, skipping\n",
                        addrToString(nextPfAddr));
            }

        } else {
            /*
             * Prefetch is already in service.  If the CPU request
             * targets the same block, mark the CPU as waiting so the
             * response path can serve it immediately — unless the
             * data is already in the cache.
             */
            DPRINTF(ARTCache, "recvTimingReq: prefetch in service\n");
            if (artPfEntry.matchBlockAddr(pkt)) {
                if (isDataInCache(pkt)) {
                    DPRINTF(ARTCache,
                            "recvTimingReq: data already in cache, "
                            "no need to wait\n");
                } else {
                    assert(artPfEntry.setCPUWaiting(pkt));
                    panic_if(!artPfEntry.ifCPUWaiting(),
                             "Failed to set CPU waiting flag.");
                    prefetchHit = true;
                    ++stats.cpuWaitEvents;
                    DPRINTF(ARTCache,
                            "recvTimingReq: CPU waiting for "
                            "in-flight prefetch\n");
                }
            }
        }
    }

    if (prefetchHit) {
        DPRINTF(ARTCache,
                "recvTimingReq: served by prefetch mechanism, "
                "skipping cache access\n");
        return;
    }

    if (directMemoryMode) {
        DPRINTF(ARTCache,
                "recvTimingReq: direct memory bypass for addr=%s\n",
                addrToString(pkt->getAddr()));
        panic_if(!bypassCacheEntry.ifTargetOpen(),
                 "Bypass target slot is not empty.");

        bypassCacheEntry.allocate(
            pkt, clockEdge(Cycles(1)), artPrefetchOrder++);
        panic_if(!bypassCacheEntry.ready(),
                 "Bypass entry not ready after allocation.");

        ++stats.bypassAccesses;
        DPRINTF(ARTCache, "recvTimingReq: bypass (bypass=%d)\n",
                stats.bypassAccesses.value());
        schedMemSideSendEvent(clockEdge());
        return;
    }

    if (enablePrefetch && !prefetchHit) {
        ++stats.prefetchMisses;
        DPRINTF(ARTCache,
                "recvTimingReq: prefetch miss "
                "(misses=%d)\n",
                stats.prefetchMisses.value());
    }

    DPRINTF(ARTCache,
            "recvTimingReq: forwarding to NoncoherentCache\n");
    NoncoherentCache::recvTimingReq(pkt);

    if (prefetchOnCacheHit && artPfEntry.ready()) {
        DPRINTF(ARTCache,
                "recvTimingReq: prefetch entry still ready after "
                "cache access, scheduling send (prefetchOnCacheHit)\n");
        schedMemSideSendEvent(clockEdge(Cycles(1)));
    }
}

// -------------------------------------------------------------------
//  Timing response path
// -------------------------------------------------------------------

void
ART::recvTimingResp(PacketPtr pkt)
{
    // --- Check bypass entry first ---
    if (bypassCacheEntry.ifInService()) {
        DPRINTF(ARTCache,
                "recvTimingResp: bypass entry in service\n");
        BypassCacheEntry *entry =
            dynamic_cast<BypassCacheEntry *>(
                pkt->findNextSenderState<BypassCacheEntry>());
        if (entry) {
            panic_if(entry != &bypassCacheEntry,
                     "Bypass entry mismatch in recvTimingResp.");
            DPRINTF(ARTCache,
                    "recvTimingResp: bypass response for addr=%s\n",
                    addrToString(entry->blkAddr));

            BypassCacheEntry *popped =
                dynamic_cast<BypassCacheEntry *>(pkt->popSenderState());
            panic_if(popped != entry,
                     "Bypass entry pop mismatch in recvTimingResp.");

            cpuSidePort.schedTimingResp(pkt, clockEdge(Cycles(1)));
            entry->deallocate();
            return;
        }
    }

    // --- Check prefetch entry ---
    if (artPfEntry.ifInService()) {
        DPRINTF(ARTCache,
                "recvTimingResp: prefetch entry in service\n");
        ARTPfQueueEntry *entry =
            dynamic_cast<ARTPfQueueEntry *>(
                pkt->findNextSenderState<ARTPfQueueEntry>());
        if (entry) {
            panic_if(entry != &artPfEntry,
                     "Prefetch entry mismatch in recvTimingResp.");

            DPRINTF(ARTCache,
                    "recvTimingResp: prefetch response for addr=%s\n",
                    addrToString(entry->blkAddr));
            ++stats.prefetchesCompleted;
            DPRINTF(ARTCache,
                    "recvTimingResp: stats "
                    "alloc=%d issued=%d completed=%d\n",
                    stats.prefetchesAllocated.value(),
                    stats.prefetchesIssued.value(),
                    stats.prefetchesCompleted.value());

            prefetchBuffer.storeData(
                entry->blkAddr, pkt->getPtr<uint8_t>());

            bool servedCPU = false;

            if (entry->ifCPUWaiting()) {
                DPRINTF(ARTCache,
                        "recvTimingResp: serving waiting CPU\n");
                entry->cpuPtr->setDataFromBlock(
                    prefetchBuffer.data.data(), prefetchBuffer.blkSize);
                entry->cpuPtr->makeTimingResponse();
                cpuSidePort.schedTimingResp(
                    entry->cpuPtr, clockEdge(Cycles(1)));
                entry->clearCPUWaiting();
                servedCPU = true;
            }

            delete pkt;
            entry->deallocate();

            /*
             * Issue a follow-up prefetch only when the current buffer
             * is free (i.e., both buffers are not simultaneously
             * occupied).
             */
            if (!currentBuffer.isValid()) {
                if (!servedCPU) {
                    currentBuffer.copyFrom(prefetchBuffer);
                    DPRINTF(ARTCache,
                            "recvTimingResp: promoted prefetch -> "
                            "current buffer\n");
                }
                prefetchBuffer.invalidate();

                if (entry->matchBlockAddr(nextPfAddr, false)) {
                    nextPfAddr = getNextSequentialAddr(nextPfAddr);
                    DPRINTF(ARTCache,
                            "recvTimingResp: advancing next prefetch "
                            "addr to %s\n", addrToString(nextPfAddr));
                }

                if (isInFlashRange(nextPfAddr)) {
                    RequestPtr req =
                        makeARTPrefetchRequest(nextPfAddr, pfBlkSize);
                    PacketPtr pfPkt =
                        makeARTPrefetchPacket(req, &artPfEntry);
                    panic_if(!pfPkt,
                             "Failed to create prefetch packet in "
                             "recvTimingResp.");
                    artPfEntry.allocate(nextPfAddr, pfPkt,
                                        clockEdge(Cycles(1)),
                                        artPrefetchOrder++);
                    ++stats.prefetchesAllocated;
                    DPRINTF(ARTCache,
                            "recvTimingResp: issued follow-up "
                            "prefetch for addr=%s (allocated=%d)\n",
                            addrToString(nextPfAddr),
                            stats.prefetchesAllocated.value());
                    schedMemSideSendEvent(clockEdge());
                } else {
                    DPRINTF(ARTCache,
                            "recvTimingResp: next prefetch addr=%s "
                            "outside flash range, skipping\n",
                            addrToString(nextPfAddr));
                }
            } else {
                DPRINTF(ARTCache, "recvTimingResp: currentBuffer valid, "
                                  "skipping follow-up prefetch\n");
            }
            return;
        }
    }

    DPRINTF(ARTCache,
            "recvTimingResp: forwarding to NoncoherentCache — "
            "pkt cmd=%s addr=%s size=%u isRead=%d isWrite=%d "
            "isResponse=%d\n",
            pkt->cmd.toString(), addrToString(pkt->getAddr()),
            pkt->getSize(), pkt->isRead(), pkt->isWrite(),
            pkt->isResponse());
    NoncoherentCache::recvTimingResp(pkt);
}

// -------------------------------------------------------------------
//  Packet send helpers
// -------------------------------------------------------------------

bool
ART::sendARTPrefetchPacket(ARTPfQueueEntry *entry)
{
    panic_if(!entry, "Null prefetch entry in sendARTPrefetchPacket.");
    panic_if(!entry->ready(),
             "Prefetch entry not ready in sendARTPrefetchPacket.");

    PacketPtr pkt = entry->getTarget()->pkt;
    panic_if(!pkt, "Null prefetch packet in sendARTPrefetchPacket.");

    if (!memSidePort.sendTimingReq(pkt)) {
        DPRINTF(ARTCache,
                "sendARTPrefetchPacket: send failed for addr=%s, "
                "will retry\n", addrToString(entry->blkAddr));
        entry->markRetrying();
        return true;
    }

    entry->markInService();
    entry->clearRetrying();
    ++stats.prefetchesIssued;
    DPRINTF(ARTCache,
            "sendARTPrefetchPacket: sent prefetch for addr=%s "
            "(issued=%d)\n",
            addrToString(entry->blkAddr), stats.prefetchesIssued.value());
    return false;
}

bool
ART::sendBypassPacket(BypassCacheEntry *entry)
{
    panic_if(!entry, "Null bypass entry in sendBypassPacket.");
    panic_if(!entry->ready(),
             "Bypass entry not ready in sendBypassPacket.");

    PacketPtr pkt = entry->getTarget()->pkt;
    panic_if(!pkt, "Null bypass packet in sendBypassPacket.");

    if (!memSidePort.sendTimingReq(pkt)) {
        DPRINTF(ARTCache,
                "sendBypassPacket: send failed for addr=%s, "
                "will retry\n", addrToString(pkt->getAddr()));
        entry->markRetrying();
        return true;
    }

    entry->markInService();
    entry->clearRetrying();
    DPRINTF(ARTCache,
            "sendBypassPacket: sent bypass for addr=%s\n",
            addrToString(pkt->getAddr()));
    return false;
}

} // namespace gem5
