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
      arriveBufferSizeLimit(p.arrive_buffer_size),
      enablePipeline(p.enable_pipeline),
      addressPhaseLatency(p.address_phase_latency),
      lastStreamId(0),
      popArriveBufferEvent(
          [this]{ popArriveBuffer(); },
          name() + ".popArriveBuffer", false, Event::Default_Pri),
      popOutputBufferEvent(
          [this]{ popOutputBuffer(); },
          name() + ".popOutputBuffer", false, Event::Default_Pri - 1),
      staleReturnEvent(
          [this]{ retryStaleReturn(); },
          name() + ".staleReturn"),
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
            "enablePipeline=%s, prefetchOnCacheHit=%s, pfBlkSize=%u, "
            "flash=[%#x, %#x]\n",
            directMemoryMode ? "true" : "false",
            enablePrefetch ? "true" : "false",
            enablePipeline ? "true" : "false",
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
               "Total prefetch responses received from memory"),
      ADD_STAT(prefetchCacheFills, statistics::units::Count::get(),
               "Prefetch responses inserted into the I-Cache"),
      ADD_STAT(prefetchCacheFillSkips, statistics::units::Count::get(),
               "Prefetch cache fills skipped (already cached or alloc failed)")
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

bool
ART::fillCacheWithPrefetchData(PacketPtr pkt)
{
    Addr blkAddr = pkt->getAddr();
    bool is_secure = pkt->isSecure();

    // Check if already in cache — skip if so
    CacheBlk *existing = tags->findBlock({blkAddr, is_secure});
    if (existing && existing->isValid()) {
        DPRINTF(ARTCache,
                "fillCacheWithPrefetchData: addr=%s already in cache, "
                "skipping\n", addrToString(blkAddr));
        ++stats.prefetchCacheFillSkips;
        return false;
    }

    // Sanity checks on the packet
    if (!pkt->hasData()) {
        DPRINTF(ARTCache,
                "fillCacheWithPrefetchData: addr=%s packet has no data, "
                "skipping\n", addrToString(blkAddr));
        ++stats.prefetchCacheFillSkips;
        return false;
    }

    if (pkt->getSize() != blkSize) {
        DPRINTF(ARTCache,
                "fillCacheWithPrefetchData: addr=%s size mismatch "
                "(pkt=%u, blkSize=%u), skipping\n",
                addrToString(blkAddr), pkt->getSize(), blkSize);
        ++stats.prefetchCacheFillSkips;
        return false;
    }

    // Check for conflicting MSHR or write buffer entries
    if (mshrQueue.findMatch(blkAddr, is_secure)) {
        DPRINTF(ARTCache,
                "fillCacheWithPrefetchData: addr=%s conflicts with "
                "MSHR entry, skipping\n", addrToString(blkAddr));
        ++stats.prefetchCacheFillSkips;
        return false;
    }
    if (writeBuffer.findMatch(blkAddr, is_secure)) {
        DPRINTF(ARTCache,
                "fillCacheWithPrefetchData: addr=%s conflicts with "
                "write buffer entry, skipping\n", addrToString(blkAddr));
        ++stats.prefetchCacheFillSkips;
        return false;
    }

    // Allocate a cache block (find victim, evict if needed, insert tag)
    PacketList writebacks;
    CacheBlk *blk = allocateBlock(pkt, writebacks);

    if (!blk) {
        DPRINTF(ARTCache,
                "fillCacheWithPrefetchData: addr=%s block allocation "
                "failed, skipping\n", addrToString(blkAddr));
        ++stats.prefetchCacheFillSkips;
        return false;
    }

    // Copy data from packet into the cache block
    pkt->writeDataToBlock(blk->data, blkSize);

    // Mark block as readable (noncoherent icache, no coherence needed)
    blk->setCoherenceBits(CacheBlk::ReadableBit);
    blk->setCoherenceBits(CacheBlk::WritableBit);

    // Set ready time
    blk->setWhenReady(clockEdge(fillLatency) +
                      pkt->headerDelay + pkt->payloadDelay);

    DPRINTF(ARTCache,
            "fillCacheWithPrefetchData: addr=%s inserted into I-Cache "
            "(fills=%d)\n",
            addrToString(blkAddr),
            stats.prefetchCacheFills.value() + 1);
    ++stats.prefetchCacheFills;

    // Handle any writebacks caused by eviction
    doWritebacks(writebacks, clockEdge(fillLatency));

    return true;
}

void
ART::serveFromBuffer(PacketPtr pkt, ARTPrefetchBuffer &buf)
{
    pkt->setData(buf.getData(pkt->getAddr(), pkt->getSize()));
    pkt->makeTimingResponse();
    // Use max of bufferHitLatency+1 and next clock edge to ensure
    // the response arrives at a cycle boundary where MinorCPU's
    // SingleStageFetch pipeline state is consistent.  The +1
    // prevents same-tick scheduling which triggers PacketQueue
    // retry assertions on long runs.
    Tick readyTick = std::max(curTick() + bufferHitLatency + 1,
                              clockEdge(Cycles(0)));
    // serveFromBuffer is called from both pipelined and serialized
    // paths. In the serialized path, processingInFlight is true at
    // this point, so the response is bound to that slot. In the
    // pipelined path, it is false (multiple buffer hits can be in
    // flight simultaneously, none binding the slot).
    pushToOutputBuffer(pkt, readyTick,
                       /*boundToInFlight=*/processingInFlight);
}

// -------------------------------------------------------------------
//  Pipeline: arrive buffer blocking helpers
// -------------------------------------------------------------------

void
ART::blockForArriveBuffer()
{
    DPRINTF(ARTCache, "blockForArriveBuffer: blocking port "
            "(arriveBuffer at capacity %d)\n", arriveBuffer.size());
    if (blocked == 0)
        cpuSidePort.setBlocked();
    blocked |= BLOCKED_ARRIVE_FULL;
}

void
ART::unblockForArriveBuffer()
{
    DPRINTF(ARTCache, "unblockForArriveBuffer: unblocking port\n");
    blocked &= ~BLOCKED_ARRIVE_FULL;
    if (blocked == 0)
        cpuSidePort.clearBlocked();
}

bool
ART::isBlockedForArriveBuffer() const
{
    return (blocked & BLOCKED_ARRIVE_FULL) != 0;
}

void
ART::maybeUnblockArriveBuffer()
{
    if (isBlockedForArriveBuffer() &&
        (arriveBufferSizeLimit == 0 ||
         arriveBuffer.size() < arriveBufferSizeLimit)) {
        unblockForArriveBuffer();
    }
}

void
ART::scheduleNextArriveBuffer()
{
    if (!arriveBuffer.empty() && !processingInFlight &&
        !popArriveBufferEvent.scheduled()) {
        DPRINTF(ARTCache, "scheduleNextArriveBuffer: scheduling at "
                "tick %d\n", curTick());
        schedule(popArriveBufferEvent, curTick());
    }
}

// -------------------------------------------------------------------
//  Pipeline: output buffer methods
// -------------------------------------------------------------------

void
ART::pushToOutputBuffer(PacketPtr pkt, Tick readyTick, bool boundToInFlight)
{
    // Enforce monotonic ordering of response delivery: a later push must
    // not drain before an earlier one. This matters when a pipelined
    // cache hit (potentially fast) is enqueued behind a buffer hit
    // (waiting bufferHitLatency).
    Tick safeReadyTick = readyTick;
    if (!outputBuffer.empty()) {
        safeReadyTick = std::max(safeReadyTick,
                                 outputBuffer.back().tick + 1);
    }
    DPRINTF(ARTCache,
            "pushToOutputBuffer: addr=%s readyTick=%d (req=%d) "
            "boundToInFlight=%d outBufLen=%lu\n",
            addrToString(pkt->getAddr()), safeReadyTick, readyTick,
            (int)boundToInFlight, outputBuffer.size());
    outputBuffer.emplace_back(pkt, safeReadyTick, boundToInFlight);
    lineTraceEvent("pushOut", pkt->getAddr(),
                   boundToInFlight ? "bound" : "free");
    scheduleOutputDrain();
}

void
ART::scheduleOutputDrain()
{
    if (!outputBuffer.empty() && !popOutputBufferEvent.scheduled()) {
        Tick outTick = std::max(outputBuffer.front().tick, curTick());
        DPRINTF(ARTCache, "scheduleOutputDrain: scheduling at tick %d\n",
                outTick);
        schedule(popOutputBufferEvent, outTick);
    }
}

void
ART::popOutputBuffer()
{
    if (outputBuffer.empty())
        return;

    DeferredPacket &dp = outputBuffer.front();
    DPRINTF(ARTCache,
            "popOutputBuffer: sending response addr=%s "
            "boundToInFlight=%d\n",
            addrToString(dp.pkt->getAddr()), (int)dp.boundToInFlight);
    lineTraceEvent("respDeliver", dp.pkt->getAddr(),
                   dp.boundToInFlight ? "bound" : "free");
    cpuSidePort.schedTimingResp(dp.pkt, curTick());

    // Only clear processingInFlight when the response that owned the
    // serializing slot drains. Pipelined buffer/cache hits never set
    // the flag and never clear it here.
    bool wasBound = dp.boundToInFlight;
    outputBuffer.pop_front();

    if (wasBound) {
        processingInFlight = false;
        DPRINTF(ARTCache,
                "popOutputBuffer: processingInFlight cleared (bound resp)\n");
        // After a serializing barrier drains, kick popArriveBuffer to
        // process whatever is queued behind it.
        scheduleNextArriveBuffer();
    } else if (!processingInFlight) {
        // Pipelined response drained while no serializing op is in
        // flight. Make sure the address-phase pipeline keeps moving if
        // arriveBuffer still has entries (popArriveBuffer schedules
        // itself when it dispatches a request, but a corner case exists
        // where outputBuffer is the only pending state).
        scheduleNextArriveBuffer();
    }

    // If more responses queued, schedule for the next one.
    if (!outputBuffer.empty() && !popOutputBufferEvent.scheduled()) {
        Tick outTick = std::max(outputBuffer.front().tick, curTick());
        reschedule(popOutputBufferEvent, outTick, true);
    }
}

void
ART::retryStaleReturn()
{
    // Drain all stale BadAddress responses into the cpu-side port's
    // RespPacketQueue via schedTimingResp. The queue then owns all
    // send/retry state machinery for these packets, uniformly with the
    // normal response path (popOutputBuffer uses schedTimingResp too).
    //
    // Previously this used raw `cpuSidePort.sendTimingResp()`, which
    // bypasses the port's queue. When that direct send failed (e.g.,
    // upstream xbar BUSY), the ART just returned and relied on "some
    // future retry" to drain. But the eventual retry arrives via the
    // port's default recvRespRetry → respQueue.retry(), which asserts
    // `waitingOnRetry` — and respQueue's waitingOnRetry was false,
    // because the failed send happened outside the queue. Observed as
    // a deterministic PacketQueue::retry assertion at tick 150,202,752
    // on bench-fpu-repeat-v{add,sub}-f32-n8 during firmware boot, when
    // a stream-change flush marked an in-flight fetch BadAddress and
    // the upstream icode_bus happened to be BUSY.
    while (!staleReturnQueue.empty()) {
        PacketPtr stale = staleReturnQueue.front();
        DPRINTF(ARTCache, "retryStaleReturn: scheduling stale response "
                "addr=%s via respQueue\n", addrToString(stale->getAddr()));
        cpuSidePort.schedTimingResp(stale, curTick());
        staleReturnQueue.pop_front();
    }
}

// -------------------------------------------------------------------
//  Timing request path
// -------------------------------------------------------------------

void
ART::recvTimingReq(PacketPtr pkt)
{
    panic_if(!pkt->isRead(), "ART only supports read requests.");

    DPRINTF(ARTCache, "recvTimingReq: addr=%s arriveBuffer=%d "
            "processingInFlight=%d\n",
            addrToString(pkt->getAddr()), arriveBuffer.size(),
            processingInFlight);

    // --- Stream ID: flush stale requests on stream change ---
    if (pkt->req->hasStreamId()) {
        uint32_t streamId = pkt->req->streamId();
        if (lastStreamId != streamId) {
            DPRINTF(ARTCache, "recvTimingReq: stream change %d -> %d, "
                    "flushing stale arrivals\n", lastStreamId, streamId);
            lastStreamId = streamId;
            while (!arriveBuffer.empty()
                   && arriveBuffer.front().pkt->req->hasStreamId()
                   && arriveBuffer.front().pkt->req->streamId()
                      != streamId) {
                PacketPtr stale = arriveBuffer.front().pkt;
                DPRINTF(ARTCache, "recvTimingReq: returning stale pkt "
                        "addr=%s streamId=%d\n",
                        addrToString(stale->getAddr()),
                        stale->req->streamId());
                stale->makeResponse();
                stale->setBadAddress();
                arriveBuffer.pop_front();
                staleReturnQueue.push_back(stale);
            }
            if (!staleReturnQueue.empty() &&
                !staleReturnEvent.scheduled())
                schedule(staleReturnEvent, curTick() + 1);
        }
    }

    // --- Admit into arriveBuffer ---
    arriveBuffer.emplace_back(pkt, curTick());

    // Schedule popArriveBuffer after address phase latency
    // (only if pipeline is idle and event not already scheduled)
    if (!processingInFlight && !popArriveBufferEvent.scheduled()) {
        Tick addrPhaseDone = curTick() + addressPhaseLatency;
        DPRINTF(ARTCache, "recvTimingReq: scheduling popArriveBuffer "
                "at tick %d\n", addrPhaseDone);
        schedule(popArriveBufferEvent,
                 std::max(addrPhaseDone, curTick()));
    }

    // Block if arriveBuffer is now at capacity
    if (arriveBufferSizeLimit != 0 &&
        arriveBuffer.size() >= arriveBufferSizeLimit) {
        blockForArriveBuffer();
    }
}

// -------------------------------------------------------------------
//  Pipeline: popArriveBuffer (address phase processing)
// -------------------------------------------------------------------

void
ART::popArriveBuffer()
{
    if (enablePipeline)
        popArriveBufferPipelined();
    else
        popArriveBufferSerialized();
}

// -------------------------------------------------------------------
//  Pipelined dispatch: pipelinable cases overlap their address phases
//  at addressPhaseLatency cadence; serializing cases set
//  processingInFlight and gate the next request via popOutputBuffer.
// -------------------------------------------------------------------

void
ART::popArriveBufferPipelined()
{
    if (processingInFlight) {
        DPRINTF(ARTCache,
                "popArriveBufferPipelined: processingInFlight, deferring\n");
        return;
    }

    // Flush stale stream packets from front (case 9 — pipelinable, free).
    while (!arriveBuffer.empty()
           && arriveBuffer.front().pkt->req->hasStreamId()
           && arriveBuffer.front().pkt->req->streamId() != lastStreamId) {
        PacketPtr stale = arriveBuffer.front().pkt;
        DPRINTF(ARTCache,
                "popArriveBufferPipelined: flushing stale pkt addr=%s\n",
                addrToString(stale->getAddr()));
        lineTraceEvent("staleDrop", stale->getAddr());
        stale->makeResponse();
        stale->setBadAddress();
        arriveBuffer.pop_front();
        staleReturnQueue.push_back(stale);
    }
    if (!staleReturnQueue.empty() && !staleReturnEvent.scheduled())
        schedule(staleReturnEvent, curTick() + 1);

    if (arriveBuffer.empty()) {
        maybeUnblockArriveBuffer();
        return;
    }

    PacketPtr pkt = arriveBuffer.front().pkt;
    arriveBuffer.pop_front();
    maybeUnblockArriveBuffer();
    lineTraceEvent("popArr", pkt->getAddr());

    // ---- Classify case (read-only on shared state) ----
    // Only buffer hits are pipelined. Cache hits go to NoncoherentCache
    // which has internal MSHR coordination — keep it on the serialized
    // path so processingInFlight gates correctly until the cache responds.
    bool curHit = enablePrefetch && !pkt->isSecure() &&
                  currentBuffer.isHit(pkt->getAddr());
    bool pfHit = !curHit && enablePrefetch && !pkt->isSecure() &&
                 prefetchBuffer.isHit(pkt->getAddr());

    DPRINTF(ARTCache,
            "popArriveBufferPipelined: addr=%s curHit=%d pfHit=%d\n",
            addrToString(pkt->getAddr()), (int)curHit, (int)pfHit);

    if (curHit) {
        // Case 1: currentBuffer hit
        ++stats.currentBufferHits;
        serveFromBuffer(pkt, currentBuffer);
        lineTraceEvent("serveCurBuf", pkt->getAddr());
        maybeIssueNextPrefetch(pkt->getAddr(), /*prefetchHit=*/true,
                               /*schedImmediate=*/true);
    } else if (pfHit) {
        // Case 2: prefetchBuffer hit. Atomically promote pf→cur BEFORE
        // serving so the next pipelined request sees the new currentBuffer.
        ++stats.prefetchBufferHits;
        currentBuffer.invalidate();
        currentBuffer.copyFrom(prefetchBuffer);
        prefetchBuffer.invalidate();
        DPRINTF(ARTCache,
                "popArriveBufferPipelined: promote prefetchBuffer to "
                "currentBuffer (pipelined)\n");
        serveFromBuffer(pkt, currentBuffer);
        lineTraceEvent("servePfBuf", pkt->getAddr());
        maybeIssueNextPrefetch(pkt->getAddr(), /*prefetchHit=*/true,
                               /*schedImmediate=*/true);
    } else {
        // Cases 4/5/6/7/8: serializing. Put the request back at front
        // of arriveBuffer and run the serialized handler, which will
        // set processingInFlight=true and dispatch correctly.
        arriveBuffer.emplace_front(pkt, curTick());
        DPRINTF(ARTCache,
                "popArriveBufferPipelined: addr=%s falls into serializing "
                "path\n", addrToString(pkt->getAddr()));
        lineTraceEvent("serFallback", pkt->getAddr());
        popArriveBufferSerialized();
        return; // serialized path handles its own continuation
    }

    // Pipelined success path: schedule next address phase.
    if (!arriveBuffer.empty() && !popArriveBufferEvent.scheduled()) {
        Tick next = curTick() + addressPhaseLatency;
        DPRINTF(ARTCache,
                "popArriveBufferPipelined: scheduling next popArriveBuffer "
                "at tick %d (+%d)\n", next, (int)addressPhaseLatency);
        schedule(popArriveBufferEvent, std::max(next, curTick()));
    }
}

// -------------------------------------------------------------------
//  Serialized dispatch (legacy behavior, used when enablePipeline=false
//  and as fallback for serializing cases from the pipelined path).
// -------------------------------------------------------------------

void
ART::popArriveBufferSerialized()
{
    // Strictly serialized: only process when previous request is done.
    if (processingInFlight) {
        DPRINTF(ARTCache, "popArriveBuffer: processingInFlight, "
                "deferring\n");
        return;
    }

    // Flush stale stream packets from front
    while (!arriveBuffer.empty()
           && arriveBuffer.front().pkt->req->hasStreamId()
           && arriveBuffer.front().pkt->req->streamId()
              != lastStreamId) {
        PacketPtr stale = arriveBuffer.front().pkt;
        DPRINTF(ARTCache, "popArriveBuffer: flushing stale pkt "
                "addr=%s\n", addrToString(stale->getAddr()));
        stale->makeResponse();
        stale->setBadAddress();
        arriveBuffer.pop_front();
        staleReturnQueue.push_back(stale);
    }
    if (!staleReturnQueue.empty() && !staleReturnEvent.scheduled())
        schedule(staleReturnEvent, curTick() + 1);

    if (arriveBuffer.empty()) {
        maybeUnblockArriveBuffer();
        return;
    }

    // Pop front entry and mark pipeline busy
    PacketPtr pkt = arriveBuffer.front().pkt;
    arriveBuffer.pop_front();
    processingInFlight = true;
    lineTraceEvent("popArrSer", pkt->getAddr());

    maybeUnblockArriveBuffer();

    DPRINTF(ARTCache, "popArriveBuffer: processing addr=%s\n",
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
                "popArriveBuffer: state curBuf=(%s) pfBuf=(%s) "
                "pfEntry=%s\n",
                curBufStr, pfBufStr, pfEntryStr);
    }

    bool prefetchHit = false;

    if (enablePrefetch && !pkt->isSecure()) {
        // --- Try to serve from the current buffer ---
        if (currentBuffer.isHit(pkt->getAddr())) {
            DPRINTF(ARTCache, "popArriveBuffer: current buffer hit\n");
            serveFromBuffer(pkt, currentBuffer);
            prefetchHit = true;
            ++stats.currentBufferHits;
            DPRINTF(ARTCache,
                    "popArriveBuffer: stats hits=%d pfHits=%d "
                    "misses=%d alloc=%d issued=%d completed=%d\n",
                    stats.currentBufferHits.value(),
                    stats.prefetchBufferHits.value(),
                    stats.prefetchMisses.value(),
                    stats.prefetchesAllocated.value(),
                    stats.prefetchesIssued.value(),
                    stats.prefetchesCompleted.value());

        } else if (prefetchBuffer.isHit(pkt->getAddr())) {
            DPRINTF(ARTCache, "popArriveBuffer: prefetch buffer hit\n");
            serveFromBuffer(pkt, prefetchBuffer);
            prefetchHit = true;
            ++stats.prefetchBufferHits;
            DPRINTF(ARTCache,
                    "popArriveBuffer: stats hits=%d pfHits=%d "
                    "misses=%d alloc=%d issued=%d completed=%d\n",
                    stats.currentBufferHits.value(),
                    stats.prefetchBufferHits.value(),
                    stats.prefetchMisses.value(),
                    stats.prefetchesAllocated.value(),
                    stats.prefetchesIssued.value(),
                    stats.prefetchesCompleted.value());

            // promote prefetchBuffer to currentBuffer
            currentBuffer.invalidate();
            currentBuffer.copyFrom(prefetchBuffer);
            prefetchBuffer.invalidate();
            DPRINTF(ARTCache,
                "popArriveBuffer: promote prefetchBuffer to "
                "currentBuffer\n");
        } else {
            currentBuffer.invalidate();
            prefetchBuffer.invalidate();
            DPRINTF(ARTCache,
                "popArriveBuffer: Invalidate both buffers "
                "because both not hit\n");
        }

        // Compute the candidate next-prefetch address.
        nextPfAddr = convertAddrToPfBlockAddr(pkt->getAddr());
        nextPfAddr = getNextSequentialAddr(nextPfAddr);
        DPRINTF(ARTCache,
            "popArriveBuffer: initial next prefetch address: %s\n",
            addrToString(nextPfAddr));

        if (!prefetchHit) {
            DPRINTF(ARTCache,
                "popArriveBuffer: CPU request misses both currentBuffer "
                "and prefetchBuffer\n"
            );

            if (isDataInCache(pkt)) {
                DPRINTF(ARTCache,
                    "popArriveBuffer: data already in cache, "
                    "will fetch from cache later\n");
            } else if (artPfEntry.ifInService()) {
                currentBuffer.invalidate();
                prefetchBuffer.invalidate();
                if (artPfEntry.matchBlockAddr(pkt)) {
                    assert(artPfEntry.setCPUWaiting(pkt));
                    panic_if(!artPfEntry.ifCPUWaiting(),
                            "Failed to set CPU waiting flag.");
                    prefetchHit = true;
                    ++stats.cpuWaitEvents;
                    DPRINTF(ARTCache,
                        "popArriveBuffer: CPU waiting for "
                        "in-flight prefetch\n");
                } else {
                    DPRINTF(ARTCache,
                        "popArriveBuffer: currently prefetching "
                        "a wrong line\n");
                }
            } else {
                currentBuffer.invalidate();
                prefetchBuffer.invalidate();
            }
        }

        // Check if need to schedule next fetch
        if (!prefetchHit || !prefetchBuffer.isValid()) {
            if (!artPfEntry.ifInService()) {
                if (artPfEntry.ifHasTarget()) {
                    DPRINTF(ARTCache,
                            "popArriveBuffer: discarding stale prefetch "
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
                             "popArriveBuffer.");

                    artPfEntry.allocate(nextPfAddr, pfPkt,
                                        clockEdge(Cycles(1)),
                                        artPrefetchOrder++);
                    ++stats.prefetchesAllocated;
                    DPRINTF(ARTCache,
                            "popArriveBuffer: allocated prefetch "
                            "for addr=%s (allocated=%d)\n",
                            addrToString(nextPfAddr),
                            stats.prefetchesAllocated.value());

                    if (prefetchHit) {
                        DPRINTF(ARTCache,
                                "popArriveBuffer: scheduling prefetch "
                                "for %s immediately\n",
                                addrToString(nextPfAddr));
                        schedMemSideSendEvent(clockEdge());
                    } else {
                        DPRINTF(ARTCache,
                                "popArriveBuffer: prefetch will be "
                                "scheduled after cache event\n");
                    }

                    panic_if(!artPfEntry.ready(),
                             "Prefetch entry not ready after allocation.");
                } else {
                    DPRINTF(ARTCache,
                            "popArriveBuffer: next prefetch addr=%s "
                            "outside flash range, skipping\n",
                            addrToString(nextPfAddr));
                }
            }
        }
    }

    if (prefetchHit) {
        DPRINTF(ARTCache,
                "popArriveBuffer: served by prefetch mechanism, "
                "skipping cache access\n");
        // processingInFlight stays true; popOutputBuffer will clear it
        return;
    }

    if (enablePrefetch) {
        ++stats.prefetchMisses;
        DPRINTF(ARTCache,
                "popArriveBuffer: prefetch miss (misses=%d)\n",
                stats.prefetchMisses.value());
    }

    if (directMemoryMode) {
        DPRINTF(ARTCache,
                "popArriveBuffer: direct memory bypass for addr=%s "
                "(pktSize=%u, pfBlkSize=%u)\n",
                addrToString(pkt->getAddr()), pkt->getSize(), pfBlkSize);
        panic_if(!bypassCacheEntry.ifTargetOpen(),
                 "Bypass target slot is not empty.");

        // Translate to pfBlkSize-aligned request (same as cache path)
        // so MinorCPU gets a full-width line back.
        assert(cachePktEntry == nullptr);
        cachePktEntry = new ARTTranslateState(pkt);
        RequestPtr bypassReq = std::make_shared<Request>(
            convertAddrToPfBlockAddr(pkt->getAddr()),
            pfBlkSize, 0, pkt->req->requestorId());
        PacketPtr bypassPkt =
            makeARTPrefetchPacket(bypassReq, cachePktEntry);

        bypassCacheEntry.allocate(
            bypassPkt, clockEdge(Cycles(1)), artPrefetchOrder++);
        panic_if(!bypassCacheEntry.ready(),
                 "Bypass entry not ready after allocation.");

        ++stats.bypassAccesses;
        DPRINTF(ARTCache, "popArriveBuffer: bypass (bypass=%d)\n",
                stats.bypassAccesses.value());
        schedMemSideSendEvent(clockEdge());
        // processingInFlight stays true; popOutputBuffer will clear it
        return;
    }

    DPRINTF(ARTCache,
            "popArriveBuffer: forwarding to NoncoherentCache\n");

    assert(cachePktEntry == nullptr);
    cachePktEntry = new ARTTranslateState(pkt);
    RequestPtr cacheReq = std::make_shared<Request>(
        convertAddrToPfBlockAddr(pkt->getAddr()),
        pfBlkSize, 0, pkt->req->requestorId());
    PacketPtr cachePkt =
        makeARTPrefetchPacket(cacheReq, cachePktEntry);
    panic_if(!cachePkt,
            "Failed to create prefetch packet in "
            "popArriveBuffer.");

    NoncoherentCache::recvTimingReq(cachePkt);

    if (prefetchOnCacheHit && artPfEntry.ready()) {
        DPRINTF(ARTCache,
                "popArriveBuffer: prefetch entry still ready after "
                "cache access, scheduling send (prefetchOnCacheHit)\n");
        schedMemSideSendEvent(clockEdge(Cycles(1)));
    }
    // processingInFlight stays true; popOutputBuffer will clear it
}

// -------------------------------------------------------------------
//  maybeIssueNextPrefetch: extracted prefetch-allocation logic.
//  Used by both pipelined and serialized paths.  Mirrors the
//  prefetch-allocation block at the end of popArriveBufferSerialized
//  but does not invalidate buffers or set processingInFlight.
// -------------------------------------------------------------------

void
ART::maybeIssueNextPrefetch(Addr reqAddr, bool prefetchHit,
                            bool schedImmediate)
{
    if (!enablePrefetch)
        return;

    nextPfAddr = convertAddrToPfBlockAddr(reqAddr);
    nextPfAddr = getNextSequentialAddr(nextPfAddr);
    DPRINTF(ARTCache,
            "maybeIssueNextPrefetch: candidate next prefetch addr=%s\n",
            addrToString(nextPfAddr));

    // Skip if we still have a valid prefetchBuffer (no need to refill).
    if (prefetchHit && prefetchBuffer.isValid())
        return;

    if (artPfEntry.ifInService())
        return;

    if (artPfEntry.ifHasTarget()) {
        DPRINTF(ARTCache,
                "maybeIssueNextPrefetch: discarding stale prefetch "
                "target addr=%s\n",
                addrToString(artPfEntry.getTarget()->pkt->getAddr()));
        delete artPfEntry.getTarget()->pkt;
        artPfEntry.deallocate();
        artPfEntry.clearCPUWaiting();
        artPfEntry.clearRetrying();
    }

    if (!isInFlashRange(nextPfAddr)) {
        DPRINTF(ARTCache,
                "maybeIssueNextPrefetch: addr=%s outside flash range, "
                "skipping\n", addrToString(nextPfAddr));
        return;
    }

    RequestPtr req = makeARTPrefetchRequest(nextPfAddr, pfBlkSize);
    PacketPtr pfPkt = makeARTPrefetchPacket(req, &artPfEntry);
    panic_if(!pfPkt,
             "Failed to create prefetch packet in maybeIssueNextPrefetch.");

    artPfEntry.allocate(nextPfAddr, pfPkt, clockEdge(Cycles(1)),
                        artPrefetchOrder++);
    ++stats.prefetchesAllocated;
    DPRINTF(ARTCache,
            "maybeIssueNextPrefetch: allocated prefetch for addr=%s "
            "(allocated=%d)\n",
            addrToString(nextPfAddr),
            stats.prefetchesAllocated.value());
    lineTraceEvent("pfAlloc", nextPfAddr);

    if (schedImmediate) {
        DPRINTF(ARTCache,
                "maybeIssueNextPrefetch: scheduling prefetch immediately\n");
        schedMemSideSendEvent(clockEdge());
    }

    panic_if(!artPfEntry.ready(),
             "Prefetch entry not ready after allocation.");
}

// -------------------------------------------------------------------
//  ARTCacheLineTrace emission
//  One row per pipeline event. Format mirrors PipelinedMemLineTrace
//  for visual diff against the no-ART path.
// -------------------------------------------------------------------

void
ART::lineTraceEvent(const char *event, Addr addr, const char *cls)
{
    DPRINTF(ARTCacheLineTrace,
            "[ART] event=%-12s addr=%#010x arrBuf=%lu outBuf=%lu "
            "inFlight=%d pfState=%s curBuf=%s pfBuf=%s cls=%s\n",
            event, addr,
            arriveBuffer.size(), outputBuffer.size(),
            (int)processingInFlight,
            artPfEntry.ifInService() ? "INSVC"
                : (artPfEntry.ifHasTarget() ? "ALLOC" : "EMPTY"),
            currentBuffer.isValid() ? "v" : "-",
            prefetchBuffer.isValid() ? "v" : "-",
            cls);
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

            // Extract original CPU packet from ARTTranslateState
            // (bypass uses same 4→8 byte translation as cache path)
            ARTTranslateState *state =
                dynamic_cast<ARTTranslateState *>(
                    pkt->popSenderState());
            if (state) {
                PacketPtr cpuPkt = state->cpuPkt;
                cpuPkt->setData(getSubBlockData(pkt, cpuPkt));
                cpuPkt->makeTimingResponse();
                cpuPkt->headerDelay = cpuPkt->payloadDelay = 0;
                DPRINTF(ARTCache,
                        "recvTimingResp: bypass translated %u-byte "
                        "response back to %u-byte CPU packet "
                        "addr=%s\n",
                        pkt->getSize(), cpuPkt->getSize(),
                        addrToString(cpuPkt->getAddr()));
                // Bypass path is serialized; this drains the in-flight
                // bypassCacheEntry, so mark the response as bound.
                pushToOutputBuffer(cpuPkt, clockEdge(Cycles(1)),
                                   /*boundToInFlight=*/true);
                state->cpuPkt = nullptr;
                delete state;
                cachePktEntry = nullptr;
                delete pkt;
            } else {
                // No translate state — send response as-is
                pushToOutputBuffer(pkt, clockEdge(Cycles(1)),
                                   /*boundToInFlight=*/true);
            }

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
                Tick readyTick = std::max(
                    curTick() + bufferHitLatency + 1,
                    clockEdge(Cycles(0)));
                // CPU was waiting for this in-flight prefetch (case 4).
                // Always serialized — mark response bound.
                pushToOutputBuffer(entry->cpuPtr, readyTick,
                                   /*boundToInFlight=*/true);
                entry->clearCPUWaiting();

                // Per RM0440 Section 3.3.4: on a miss (CPU missed
                // curBuf, pfBuf, and I-Cache), the line read is copied
                // into the instruction cache memory.
                // Skip in direct memory mode — cache is bypassed.
                if (!directMemoryMode) {
                    DPRINTF(ARTCache,
                            "recvTimingResp: filling I-Cache with prefetch "
                            "data for addr=%s\n",
                            addrToString(entry->blkAddr));
                    fillCacheWithPrefetchData(pkt);
                }
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
    // Cache hits called from the pipelined path leave processingInFlight
    // false; called from the serialized (legacy) path it is true. Use
    // that to determine whether this response drains the in-flight slot.
    pushToOutputBuffer(cpuPkt, request_time,
                       /*boundToInFlight=*/processingInFlight);

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
            // MSHR completion of a prefetch / cache miss is part of the
            // serialized path (case 7). Mark response bound.
            pushToOutputBuffer(tgt_pkt, completion_time,
                               /*boundToInFlight=*/true);

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
