/**
 * @file
 * Implementation of the ART (Adaptive Real-Time) accelerator cache.
 */

#include "mem/cache/art.hh"

#include "mem/cache/cache_blk.hh"
#include "mem/cache/mshr.hh"

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
      bufferHitLatency(p.buffer_hit_latency),
      flashStartAddr(p.flash_start_addr),
      flashEndAddr(p.flash_end_addr),
      prefetchBuffer(pfBlkSize),
      currentBuffer(pfBlkSize),
      nextPfAddr(0),
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
    CacheBlk *blk = tags->findBlock({convertAddrToPfBlockAddr(pkt->getAddr()),
                                                            pkt->isSecure()});
    return blk && blk->isValid();
}

void
ART::serveFromBuffer(PacketPtr pkt, ARTPrefetchBuffer &buf)
{
    pkt->setData(buf.getData(pkt->getAddr(), pkt->getSize()));
    pkt->makeTimingResponse();
    cpuSidePort.schedTimingResp(pkt, clockEdge(bufferHitLatency));
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

            // in here promote prefetchBuffer to currentBuffer for future
            // accesses
            currentBuffer.invalidate();
            currentBuffer.copyFrom(prefetchBuffer);
            prefetchBuffer.invalidate();
            DPRINTF(ARTCache,
                "recvTimingReq: promote prefetchBuffer to currentBuffer\n");
        }

        // Compute the candidate next-prefetch address.
        nextPfAddr = convertAddrToPfBlockAddr(pkt->getAddr());
        nextPfAddr = getNextSequentialAddr(nextPfAddr);
        DPRINTF(ARTCache,
            "recvTimingReq: initial next prefetch address: %s\n",
            addrToString(nextPfAddr));

        // We will only issue new prefetch under 2 cases
        // 1) the current cpu request missed in both currentBuffer and
        // prefetchBuffer
        // 2) the current prefetchBuffer is empty (invalid)

        if (!prefetchHit) {
            DPRINTF(ARTCache,
                "recvTimingReq: CPU request misses both currentBuffer and "
                "prefetchBuffer\n"
            );

            // There are 4 cases
            // 1) cache hits
            // 2) artPfEntry is currently prefetching the next line
            // 3) artPfEntry is currently prefetching a random address
            // 4) artPfEntry is not inService
            if (isDataInCache(pkt)) {
                DPRINTF(ARTCache,
                    "recvTimingReq: data already in cache, "
                    "will fetch from cache later\n");
            } else if (artPfEntry.ifInService()) {
                currentBuffer.invalidate();
                prefetchBuffer.invalidate();
                if (artPfEntry.matchBlockAddr(pkt)) {
                    assert(artPfEntry.setCPUWaiting(pkt));
                    panic_if(!artPfEntry.ifCPUWaiting(),
                            "Failed to set CPU waiting flag.");
                    // Don't need to reschedule
                    prefetchHit = true;
                    ++stats.cpuWaitEvents;
                    DPRINTF(ARTCache,
                        "recvTimingReq: CPU waiting for "
                        "in-flight prefetch \n");
                } else {
                    DPRINTF(ARTCache,
                        "recvTimingReq: currently prefetching a wrong line\n");
                }
            } else {
                currentBuffer.invalidate();
                prefetchBuffer.invalidate();
            }
        }

        // Check if need to schedule next fetch
        if (!prefetchHit || !prefetchBuffer.isValid()) {
            // need to schedule next prefetch
            if (!artPfEntry.ifInService()) {
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

    assert(cachePktEntry == nullptr);
    cachePktEntry = new ARTTranslateState(pkt);
    RequestPtr cacheReq = std::make_shared<Request>(
        convertAddrToPfBlockAddr(pkt->getAddr()),
        pfBlkSize, 0, pkt->req->requestorId());
    PacketPtr cachePkt =
        makeARTPrefetchPacket(cacheReq, cachePktEntry);
    panic_if(!cachePkt,
            "Failed to create prefetch packet in "
            "recvTimingReq.");

    NoncoherentCache::recvTimingReq(cachePkt);

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

            if (entry->ifCPUWaiting()) {
                DPRINTF(ARTCache,
                        "recvTimingResp: serving waiting CPU\n");
                entry->cpuPtr->setData(
                    getSubBlockData(pkt, entry->cpuPtr));
                entry->cpuPtr->makeTimingResponse();
                cpuSidePort.schedTimingResp(
                    entry->cpuPtr, clockEdge(bufferHitLatency));
                entry->clearCPUWaiting();
            }

            delete pkt;
            entry->deallocate();

            /*
             * Issue a follow-up prefetch only when the current buffer
             * is free (i.e., both buffers are not simultaneously
             * occupied).
             */
            if (!currentBuffer.isValid()) {
                currentBuffer.copyFrom(prefetchBuffer);
                DPRINTF(ARTCache,
                    "recvTimingResp: promoted prefetch -> "
                    "current buffer\n");
                if (currentBuffer.addr == nextPfAddr) {
                    nextPfAddr = getNextSequentialAddr(nextPfAddr);
                }
                DPRINTF(ARTCache,
                        "recvTimingResp: advancing next prefetch "
                        "addr to %s\n", addrToString(nextPfAddr));
                prefetchBuffer.invalidate();

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

    // serviceMSHRTargets (overridden above) handles the
    // ARTTranslateState swap and CPU response.
    NoncoherentCache::recvTimingResp(pkt);
}

// -------------------------------------------------------------------
//  Cache hit handling (override for 4-byte → 8-byte translation)
// -------------------------------------------------------------------

void
ART::handleTimingReqHit(PacketPtr pkt, CacheBlk *blk, Tick request_time)
{
    ARTTranslateState *state =
        dynamic_cast<ARTTranslateState *>(
            pkt->findNextSenderState<ARTTranslateState>());

    if (!state) {
        // Not a translated packet, use default path.
        BaseCache::handleTimingReqHit(pkt, blk, request_time);
        return;
    }

    // Extract the original CPU packet.
    pkt->popSenderState();
    PacketPtr cpuPkt = state->cpuPkt;

    // Copy the right 4 bytes from the 8-byte cachePkt into the CPU packet.
    cpuPkt->setData(getSubBlockData(pkt, cpuPkt));
    cpuPkt->makeTimingResponse();

    cpuPkt->headerDelay = cpuPkt->payloadDelay = 0;
    cpuSidePort.schedTimingResp(cpuPkt, request_time);

    state->cpuPkt = nullptr;
    delete state;
    cachePktEntry = nullptr;
    delete pkt;
}

// -------------------------------------------------------------------
//  MSHR target servicing (override for 4-byte → 8-byte translation)
// -------------------------------------------------------------------

void
ART::serviceMSHRTargets(MSHR *mshr, const PacketPtr pkt, CacheBlk *blk)
{
    if (pkt->cmd == MemCmd::LockedRMWWriteResp) {
        QueueEntry::Target *initial_tgt = mshr->getTarget();
        assert(initial_tgt->pkt->cmd == MemCmd::LockedRMWReadReq);
        delete initial_tgt->pkt;
        initial_tgt->pkt = nullptr;
        mshr->popTarget();
    }

    const int initial_offset = mshr->hasTargets() ?
        mshr->getTarget()->pkt->getOffset(blkSize) : 0;

    MSHR::TargetList targets = mshr->extractServiceableTargets(pkt);
    bool from_core = false;
    bool from_pref = false;

    for (auto &target: targets) {
        Packet *tgt_pkt = target.pkt;

        switch (target.source) {
          case MSHR::Target::FromCPU:
          {
            from_core = true;

            // If this target has an ARTTranslateState, swap in the
            // original CPU packet so satisfyRequest copies the right
            // bytes and the CPU gets back its own packet.
            PacketPtr cachePktToDelete = nullptr;
            ARTTranslateState *state =
                dynamic_cast<ARTTranslateState *>(
                    tgt_pkt->findNextSenderState<ARTTranslateState>());
            if (state) {
                tgt_pkt->popSenderState();
                cachePktToDelete = tgt_pkt;
                tgt_pkt = state->cpuPkt;
                target.pkt = tgt_pkt;
                state->cpuPkt = nullptr;
                delete state;
                cachePktEntry = nullptr;
            }

            Tick completion_time;
            completion_time = pkt->headerDelay;

            satisfyRequest(tgt_pkt, blk);

            int transfer_offset;
            transfer_offset =
                tgt_pkt->getOffset(blkSize) - initial_offset;
            if (transfer_offset < 0) {
                transfer_offset += blkSize;
            }
            completion_time += clockEdge(responseLatency) +
                (transfer_offset ? pkt->payloadDelay : 0);

            assert(tgt_pkt->req->requestorId() < system->maxRequestors());
            BaseCache::stats.cmdStats(tgt_pkt)
                .missLatency[tgt_pkt->req->requestorId()] +=
                    completion_time - target.recvTime;

            if (tgt_pkt->cmd == MemCmd::LockedRMWReadReq) {
                mshr->updateLockedRMWReadTarget(tgt_pkt);
                blk->clearCoherenceBits(CacheBlk::WritableBit);
                blk->clearCoherenceBits(CacheBlk::ReadableBit);
            }

            tgt_pkt->makeTimingResponse();
            if (pkt->isError())
                tgt_pkt->copyError(pkt);

            tgt_pkt->headerDelay = tgt_pkt->payloadDelay = 0;
            cpuSidePort.schedTimingResp(tgt_pkt, completion_time);

            if (cachePktToDelete)
                delete cachePktToDelete;

            break;
          }

          case MSHR::Target::FromPrefetcher:
            assert(tgt_pkt->cmd == MemCmd::HardPFReq);
            from_pref = true;
            delete tgt_pkt;
            break;

          default:
            panic("Illegal target->source enum %d\n", target.source);
        }
    }

    if (blk && !from_core && from_pref) {
        blk->setPrefetched();
    }

    assert(mshr->getNumTargets() == 0 || mshr->hasLockedRMWReadTarget());
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
