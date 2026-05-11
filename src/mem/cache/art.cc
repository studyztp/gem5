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
      prefetchBufferHitLatency(p.prefetch_buffer_hit_latency),
      flashResponseLatency(p.flash_response_latency),
      portAhbBufferSize(p.port_ahb_buffer_size),
      portAhbBufferLatency(p.port_ahb_buffer_latency),
      flashStartAddr(p.flash_start_addr),
      flashEndAddr(p.flash_end_addr),
      arriveBufferSizeLimit(p.arrive_buffer_size),
      // Stage 2 (issue 2026-05-10-art-bypass-vs-no-art-divergence):
      // single-deep readyToFireBuffer matches legacy 1-request-at-a-time
      // pipeline exactly.  Stage 3 raises this to maxOutstandingRequests
      // for AHB-Lite back-to-back pipelining.
      readyToFireBufferSizeLimit(1),
      enablePipeline(p.enable_pipeline),
      maxOutstandingRequests(p.max_outstanding_requests),
      psmCompatibleBypass(p.psm_compatible_bypass),
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
      ahbBuffer(portAhbBufferSize ? portAhbBufferSize : 1),
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

    // Stage-1 scaffolding: size the slot vector.  0 → treat as 1
    // (legacy behavior); >0 → that many concurrent requests allowed
    // (Stage 3+).
    inFlightSlots.resize(maxOutstandingRequests > 0
                         ? maxOutstandingRequests : 1);

    DPRINTF(ARTCache,
            "ART created: directMemoryMode=%s, enablePrefetch=%s, "
            "enablePipeline=%s, prefetchOnCacheHit=%s, pfBlkSize=%u, "
            "ahbBufSize=%u, ahbBufLat=%lluTicks, flash=[%#x, %#x], "
            "maxOutstandingRequests=%u, psmCompatibleBypass=%s\n",
            directMemoryMode ? "true" : "false",
            enablePrefetch ? "true" : "false",
            enablePipeline ? "true" : "false",
            prefetchOnCacheHit ? "true" : "false", pfBlkSize,
            portAhbBufferSize, (unsigned long long)portAhbBufferLatency,
            flashStartAddr, flashEndAddr,
            maxOutstandingRequests,
            psmCompatibleBypass ? "true" : "false");

    // Stage-0 wiring check: emit one event per new flag so we can
    // verify the flags are registered and visible from the runscript.
    DPRINTF(ARTPipeline,
            "ART pipeline scaffolding wired (max_outstanding=%u, "
            "stage-0 build).\n",
            maxOutstandingRequests);
    DPRINTF(ARTBuffers,
            "ART buffers configured: ahb_size=%u ahb_lat=%lluTicks "
            "pfBlk=%u\n",
            portAhbBufferSize, (unsigned long long)portAhbBufferLatency,
            pfBlkSize);
    DPRINTF(ARTPrefetch,
            "ART prefetcher: enable=%s on_cache_hit=%s\n",
            enablePrefetch ? "true" : "false",
            prefetchOnCacheHit ? "true" : "false");
    DPRINTF(ARTBypass,
            "ART bypass mode: direct_memory_mode=%s "
            "psm_compatible=%s (active when both prefetch and cache "
            "are off; stage-3+ enables PSM-equivalent fast path)\n",
            directMemoryMode ? "true" : "false",
            psmCompatibleBypass ? "true" : "false");
}

// -------------------------------------------------------------------
//  Stage-1 in-flight slot scaffolding
//  (issue 2026-05-10-art-bypass-vs-no-art-divergence)
// -------------------------------------------------------------------

int
ART::slotAcquire(PacketPtr pkt, InFlightStage stage)
{
    for (size_t i = 0; i < inFlightSlots.size(); ++i) {
        if (inFlightSlots[i].stage == InFlightStage::Empty) {
            inFlightSlots[i].pkt = pkt;
            inFlightSlots[i].admitTick = curTick();
            inFlightSlots[i].streamId = pkt && pkt->req->hasStreamId()
                                      ? pkt->req->streamId() : 0;
            inFlightSlots[i].stage = stage;
            return static_cast<int>(i);
        }
    }
    // No free slot.  Stage 1 with max_outstanding=1 should never reach
    // here because processingInFlight already throttles to 1 concurrent.
    DPRINTF(ARTPipeline,
            "slotAcquire: no free slot (capacity=%u); pkt=%s\n",
            (unsigned)inFlightSlots.size(),
            pkt ? addrToString(pkt->getAddr()).c_str() : "<null>");
    return -1;
}

void
ART::slotRelease(int idx)
{
    if (idx < 0 || idx >= (int)inFlightSlots.size())
        return;
    inFlightSlots[idx].pkt = nullptr;
    inFlightSlots[idx].admitTick = 0;
    inFlightSlots[idx].streamId = 0;
    inFlightSlots[idx].stage = InFlightStage::Empty;
}

unsigned
ART::slotsAvailable() const
{
    unsigned n = 0;
    for (const auto &s : inFlightSlots) {
        if (s.stage == InFlightStage::Empty) ++n;
    }
    return n;
}

int
ART::slotFindByPkt(PacketPtr pkt) const
{
    for (size_t i = 0; i < inFlightSlots.size(); ++i) {
        if (inFlightSlots[i].pkt == pkt &&
            inFlightSlots[i].stage != InFlightStage::Empty)
            return static_cast<int>(i);
    }
    return -1;
}

int
ART::slotFirstInDataPhase() const
{
    for (size_t i = 0; i < inFlightSlots.size(); ++i) {
        if (inFlightSlots[i].stage == InFlightStage::DataPhase ||
            inFlightSlots[i].stage == InFlightStage::Response)
            return static_cast<int>(i);
    }
    return -1;
}

std::string
ART::slotsSnapshot() const
{
    std::ostringstream os;
    os << "slots[" << inFlightSlots.size() << "]={";
    bool first = true;
    for (size_t i = 0; i < inFlightSlots.size(); ++i) {
        if (!first) os << ",";
        first = false;
        const auto &s = inFlightSlots[i];
        const char *st;
        switch (s.stage) {
            case InFlightStage::Empty:        st = "EMPTY"; break;
            case InFlightStage::Admitted:     st = "ADM";   break;
            case InFlightStage::AddressPhase: st = "ADDR";  break;
            case InFlightStage::DataPhase:    st = "DATA";  break;
            case InFlightStage::Response:     st = "RESP";  break;
            default:                          st = "?";     break;
        }
        os << i << ":" << st;
        if (s.pkt) os << "@" << std::hex << s.pkt->getAddr() << std::dec;
    }
    os << "}";
    return os.str();
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
    // Use max of prefetchBufferHitLatency+1 and next clock edge to
    // ensure the response arrives at a cycle boundary where the
    // F-stage pipeline state is consistent.  The +1 prevents same-tick
    // scheduling which triggers PacketQueue retry assertions on long
    // runs.  This path only runs when enablePrefetch=true (Cases
    // B1/B2 in popReadyToFireBuffer) — bypass mode never reaches here.
    Tick readyTick = std::max(curTick() + prefetchBufferHitLatency + 1,
                              clockEdge(Cycles(0)));
    // serveFromBuffer is called from both pipelined and serialized
    // paths. In the serialized path, processingInFlight is true at
    // this point, so the response is bound to that slot. In the
    // pipelined path, it is false (multiple buffer hits can be in
    // flight simultaneously, none binding the slot).
    pushToOutputBuffer(pkt, readyTick,
                       /*boundToInFlight=*/processingInFlight);
    // Mirror the just-served line into the port AHB buffer so the
    // next sequential access within the same line can serve at one
    // AHB data-phase (portAhbBufferLatency) without re-traversing
    // the buffer/cache lookup. No-op when AHB buffer is disabled.
    updateAHBBufferFromLine(buf);
}

void
ART::updateAHBBufferFromLine(const ARTPrefetchBuffer &src,
                             uint32_t streamId)
{
    if (portAhbBufferSize == 0 || !src.isValid())
        return;
    // The AHB buffer mirrors a line of size portAhbBufferSize. The
    // source buffer (curBuf / pfBuf) is pfBlkSize bytes; we copy the
    // entire line if sizes match, else just the portAhbBufferSize
    // sub-block aligned to the line.
    panic_if(portAhbBufferSize > pfBlkSize,
             "port_ahb_buffer_size (%u) cannot exceed pf_blk_size (%u)",
             portAhbBufferSize, pfBlkSize);
    Addr line_addr = src.addr & ~(Addr(portAhbBufferSize) - 1);
    // Copy the matching sub-block from src.data into ahbBuffer.
    Addr offset = line_addr - src.addr;
    ahbBuffer.storeData(line_addr,
                        const_cast<ARTPrefetchBuffer&>(src).data.data()
                            + offset);
    ahbBufferedStreamId = streamId;
    DPRINTF(ARTCache, "updateAHBBufferFromLine: ahbBuffer <- addr=%s "
            "(size=%u streamId=%u)\n",
            addrToString(line_addr), portAhbBufferSize, streamId);
    lineTraceEvent("ahbBufFill", line_addr, "fromBuf");
}

void
ART::updateAHBBufferFromRaw(Addr blk_addr, const uint8_t *src_data,
                            uint32_t streamId)
{
    if (portAhbBufferSize == 0 || src_data == nullptr)
        return;
    Addr line_addr = blk_addr & ~(Addr(portAhbBufferSize) - 1);
    ahbBuffer.storeData(line_addr, src_data);
    ahbBufferedStreamId = streamId;
    DPRINTF(ARTCache, "updateAHBBufferFromRaw: ahbBuffer <- addr=%s "
            "(size=%u streamId=%u)\n",
            addrToString(line_addr), portAhbBufferSize, streamId);
    lineTraceEvent("ahbBufFill", line_addr, "fromRaw");
}

void
ART::serveFromAHBBuffer(PacketPtr pkt)
{
    pkt->setData(ahbBuffer.getData(pkt->getAddr(), pkt->getSize()));
    pkt->makeTimingResponse();
    // AHB-buffer hit pays exactly portAhbBufferLatency — mirrors PSM's
    // `outputBuffer.tick = curTick + bufferHitLatency` (PSM's name for
    // the same concept; PSM has no separate prefetch-buffer concept).
    // The
    // earlier `+1` and `clockEdge(Cycles(0))` introduced ~744 ticks
    // of extra delay per hit on the system clock domain (system.clk
    // = 5882 ticks/cy), which shifted the F-stage's fetch FIFO
    // timing enough to cause the F-stage to issue speculative fetches
    // earlier than PSM, creating a 2-cy/iter compounding overhead on
    // branch-heavy benches.  See timeline 2026-05-10 line-trace
    // analysis of bench-bp_forward.
    Tick readyTick = curTick() + portAhbBufferLatency;
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
    // Consider both arriveBuffer (waiting for address phase) and
    // readyToFireBuffer (post address-phase, may be stuck behind
    // processingInFlight).  Stage 2 introduced the latter; without
    // checking it here, an entry that arrived during processingInFlight
    // would never re-fire popReadyToFireBuffer after the lock clears.
    if ((!arriveBuffer.empty() || !readyToFireBuffer.empty()) &&
        !processingInFlight &&
        !popArriveBufferEvent.scheduled()) {
        // Mirror PSM: if outputBuffer has a pending entry (e.g., an
        // AHB-hit response queued at +portAhbBufferLatency), schedule
        // popArriveBuffer at the output drain tick so the next
        // data-phase dispatch waits for it to deliver.  Stage 3.
        Tick when = curTick();
        if (!outputBuffer.empty() && outputBuffer.front().tick > when) {
            when = outputBuffer.front().tick;
        }
        DPRINTF(ARTCache, "scheduleNextArriveBuffer: scheduling at "
                "tick %d (arrBuf=%d rtf=%d outBufFront=%d)\n",
                (int)when, (int)arriveBuffer.size(),
                (int)readyToFireBuffer.size(),
                (int)(outputBuffer.empty() ? -1
                      : outputBuffer.front().tick));
        schedule(popArriveBufferEvent, when);
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
    // (waiting portAhbBufferLatency).
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

    // Stale stream-id drop: mirror PSM's popOutputBuffer
    // (gem5/src/mem/pipelined_simple_mem.cc:138).  If the in-flight
    // request's streamId no longer matches the port's current
    // streamId (lastStreamId), the F-stage has redirected and no
    // longer needs this response.  Silently drop it — do NOT
    // sendTimingResp (the F-stage would just discard via its own
    // streamId filtering, adding wasted port-queue events), do NOT
    // fill the AHB buffer (the line is from a wrong-path stream and
    // would create false hits for the new stream's first fetch).
    // PSM emits no lineTrace event for the drop, so neither do we.
    if (dp.pkt->req->hasStreamId() &&
            dp.pkt->req->streamId() != lastStreamId) {
        DPRINTF(ARTCache,
                "popOutputBuffer: dropping stale response addr=%s "
                "streamId=%u (current=%u)\n",
                addrToString(dp.pkt->getAddr()),
                dp.pkt->req->streamId(), lastStreamId);
        PacketPtr stalePkt = dp.pkt;
        bool wasBound = dp.boundToInFlight;
        outputBuffer.pop_front();
        // For case F unbound responses, release the slot here so the
        // next case-F dispatch can proceed.  For bound responses,
        // the legacy serialized path's processingInFlight bookkeeping
        // handles slot release (kept consistent with the wasBound
        // branch below).
        if (!wasBound) {
            int relIdx = slotFindByPkt(stalePkt);
            if (relIdx >= 0)
                slotRelease(relIdx);
        }
        delete stalePkt;
        if (wasBound) {
            processingInFlight = false;
            scheduleNextArriveBuffer();
        } else if (!processingInFlight) {
            scheduleNextArriveBuffer();
        }
        if (!outputBuffer.empty() && !popOutputBufferEvent.scheduled()) {
            Tick outTick = std::max(outputBuffer.front().tick, curTick());
            reschedule(popOutputBufferEvent, outTick, true);
        }
        return;
    }

    DPRINTF(ARTCache,
            "popOutputBuffer: sending response addr=%s "
            "boundToInFlight=%d hasLineData=%d\n",
            addrToString(dp.pkt->getAddr()), (int)dp.boundToInFlight,
            (int)dp.hasLineData);
    // Case-F deferred AHB-buffer fill: apply the line data at the
    // response-delivery tick.  This MUST happen before
    // schedTimingResp so the within-line follow-up (which may be in
    // the popArriveBuffer scheduled at this same tick by
    // scheduleNextArriveBuffer) sees the updated AHB buffer when it
    // checks AHB-hit in popReadyToFireBuffer.
    if (dp.hasLineData) {
        updateAHBBufferFromRaw(dp.lineBlockAddr,
                               dp.lineData.data(),
                               dp.lineStreamId);
    }
    lineTraceEvent("respDeliver", dp.pkt->getAddr(),
                   dp.boundToInFlight ? "bound" : "free");
    cpuSidePort.schedTimingResp(dp.pkt, curTick());

    // Only clear processingInFlight when the response that owned the
    // serializing slot drains. Pipelined buffer/cache hits never set
    // the flag and never clear it here.
    bool wasBound = dp.boundToInFlight;
    // Capture pkt/addr before pop_front invalidates `dp` (it's a
    // reference into outputBuffer.front()).  Without this, downstream
    // DPRINTFs and slotFindByPkt() dereference a dangling pointer.
    // Issue 2026-05-10-art-bypass-vs-no-art-divergence.
    PacketPtr drainedPkt = dp.pkt;
    Addr drainedAddr = drainedPkt ? drainedPkt->getAddr() : 0;
    outputBuffer.pop_front();

    if (wasBound) {
        processingInFlight = false;
        // Stage-1 scaffolding: release the slot that backed this
        // serializing request.  With max_outstanding=1 this is 1:1 with
        // processingInFlight; later stages will track per-packet.
        int relIdx = slotFirstInDataPhase();
        if (relIdx >= 0) {
            DPRINTF(ARTPipeline,
                    "popOutputBuffer: slot[%d] release (bound resp pkt=%s); "
                    "%s\n",
                    relIdx, addrToString(drainedAddr),
                    slotsSnapshot().c_str());
            slotRelease(relIdx);
        }
        DPRINTF(ARTCache,
                "popOutputBuffer: processingInFlight cleared (bound resp)\n");
        // After a serializing barrier drains, kick popArriveBuffer to
        // process whatever is queued behind it.
        scheduleNextArriveBuffer();
    } else if (!processingInFlight) {
        // Pipelined response drained while no serializing op is in
        // flight.  Two sub-cases:
        //   (a) Case-A (AHB hit) or Case-B1/B2 (curBuf/pfBuf hit) —
        //       no slot acquired, nothing to release.
        //   (b) Case-F (PSM-internal flash dispatch) — slot is in
        //       DataPhase, must be released here so the next case-F
        //       dispatch can proceed.  Find by pkt match.
        int relIdx = slotFindByPkt(drainedPkt);
        if (relIdx >= 0) {
            DPRINTF(ARTPipeline,
                    "popOutputBuffer: slot[%d] release (unbound resp "
                    "pkt=%s, case F); %s\n",
                    relIdx, addrToString(drainedAddr),
                    slotsSnapshot().c_str());
            slotRelease(relIdx);
        }
        // Make sure the address-phase pipeline keeps moving if
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
                    "dropping stale arrivals\n", lastStreamId, streamId);
            lastStreamId = streamId;
            while (!arriveBuffer.empty()
                   && arriveBuffer.front().pkt->req->hasStreamId()
                   && arriveBuffer.front().pkt->req->streamId()
                      != streamId) {
                PacketPtr stale = arriveBuffer.front().pkt;
                DPRINTF(ARTCache, "recvTimingReq: dropping stale pkt "
                        "addr=%s streamId=%d\n",
                        addrToString(stale->getAddr()),
                        stale->req->streamId());
                // Mirror PSM: silently delete the stale packet without
                // sending a BadAddress response.  The CPU's F-stage
                // tracks fetches by streamId and discards responses
                // whose streamId no longer matches the current stream;
                // sending a BadAddress reply just adds 1+ ticks of port
                // queueing for no functional benefit.
                lineTraceEvent("staleDrop", stale->getAddr());
                arriveBuffer.pop_front();
                delete stale;
            }
        }
    }

    // --- Admit into arriveBuffer ---
    arriveBuffer.emplace_back(pkt, curTick());

    // Schedule popArriveBuffer after address phase latency
    // (only if pipeline is idle and event not already scheduled).
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
//  Pipelined dispatch (Stage 2 of the PSM-equivalence refactor).
//  Mirrors the structure of PipelinedSimpleMemory::popArriveBuffer:
//
//      arriveBuffer --(address phase)--> readyToFireBuffer --(dispatch)
//
//  popArriveBufferPipelined: stream-flush arriveBuffer, promote one
//  entry to readyToFireBuffer, then call popReadyToFireBuffer inline.
//  popReadyToFireBuffer: runs the actual case dispatch (AHB hit,
//  curBuf/pfBuf hit, AHB-miss fallback to serialized).
//
//  Stage 2 keeps the structural single-slot serialization for cases
//  C/D/E/F (cache + bypass) by routing the AHB-miss fallback through
//  popArriveBufferSerialized.  Stage 3 will replace that with a
//  PSM-equivalent direct flash path so the bypass mode produces
//  bit-for-bit identical cycles to the no-ART board.
// -------------------------------------------------------------------

void
ART::popArriveBufferPipelined()
{
    // Flush stale stream packets from arriveBuffer (Case G, half 1).
    // The other half — flushing stale entries already promoted to
    // readyToFireBuffer — happens inside popReadyToFireBuffer.
    while (!arriveBuffer.empty()
           && arriveBuffer.front().pkt->req->hasStreamId()
           && arriveBuffer.front().pkt->req->streamId() != lastStreamId) {
        PacketPtr stale = arriveBuffer.front().pkt;
        DPRINTF(ARTCache,
                "popArriveBufferPipelined: dropping stale pkt addr=%s\n",
                addrToString(stale->getAddr()));
        lineTraceEvent("staleDrop", stale->getAddr());
        // Silent delete (mirrors PSM) — see recvTimingReq for rationale.
        arriveBuffer.pop_front();
        delete stale;
    }

    // Promote one entry from arriveBuffer to readyToFireBuffer if the
    // latter has room.  This models the address phase completing.
    if (!arriveBuffer.empty() &&
            (readyToFireBufferSizeLimit == 0
             || readyToFireBuffer.size() < readyToFireBufferSizeLimit)) {
        Addr movedAddr = arriveBuffer.front().pkt->getAddr();
        readyToFireBuffer.push_back(arriveBuffer.front());
        readyToFireBuffer.back().tick = curTick();
        arriveBuffer.pop_front();
        maybeUnblockArriveBuffer();
        DPRINTF(ARTPipeline,
                "popArriveBuf: addr=%s arriveBuffer→readyToFireBuffer "
                "(arrBuf=%d rtf=%d) %s\n",
                addrToString(movedAddr),
                (int)arriveBuffer.size(),
                (int)readyToFireBuffer.size(),
                slotsSnapshot().c_str());
        lineTraceEvent("popArr", movedAddr);
    } else if (arriveBuffer.empty() && readyToFireBuffer.empty()) {
        maybeUnblockArriveBuffer();
        return;
    }

    // Run the dispatcher.  popReadyToFireBuffer honors processingInFlight
    // (Stage 2 still gates dispatch on the single-slot lock); if it
    // defers, the entry stays in readyToFireBuffer for popOutputBuffer
    // to re-fire via scheduleNextArriveBuffer.
    popReadyToFireBuffer();

    // Trigger-based scheduling for the next popArriveBuffer.
    //
    // Three structural blockers; skip the schedule and let an event
    // chain trigger us when the blocker clears:
    //
    //   (a) processingInFlight (cases C/D/E serialized lock) —
    //       popOutputBuffer's drain will re-fire us via
    //       scheduleNextArriveBuffer when the lock clears.
    //   (b) data phase busy AND no output pending (case F flash in
    //       flight, no AHB-hit queued) — recvTimingResp's slotRelease
    //       → popOutputBuffer drain → scheduleNextArriveBuffer will
    //       re-fire us when the response arrives.
    //   (c) data phase busy AND output pending (case F flash in
    //       flight + AHB-hit ahead in outputBuffer at
    //       +portAhbBufferLatency) — schedule at the output drain tick
    //       so we don't pipeline the next miss dispatch ahead of
    //       the AHB hit serve (PSM serialization).
    //
    // Otherwise schedule at curTick + addressPhaseLatency (the
    // address phase of the next request can begin while the data
    // phase is idle).
    bool rtfHasRoom = readyToFireBufferSizeLimit == 0
                      || readyToFireBuffer.size() < readyToFireBufferSizeLimit;
    bool dispatcherCanProgress = !readyToFireBuffer.empty()
                                 && slotFirstInDataPhase() < 0;
    bool dataPhaseBusy = slotFirstInDataPhase() >= 0;
    bool outputPending = !outputBuffer.empty()
                         && outputBuffer.front().tick > curTick();

    if (!arriveBuffer.empty() && !processingInFlight &&
            !popArriveBufferEvent.scheduled()) {
        if (dataPhaseBusy && !outputPending) {
            // Pure case-F flash in flight — wait for slotRelease
            // trigger via the recvTimingResp → popOutputBuffer chain.
            // No fixed-latency schedule here; the trigger fires when
            // the response actually arrives, irrespective of how the
            // dispatch's latency was determined (flash, prefetch, or
            // cache).
            DPRINTF(ARTPipeline,
                    "popArriveBuf: trigger-wait (arrBuf=%d slot busy, "
                    "no output pending) %s\n",
                    (int)arriveBuffer.size(), slotsSnapshot().c_str());
        } else if (!(rtfHasRoom || dispatcherCanProgress)) {
            DPRINTF(ARTPipeline,
                    "popArriveBuf: NOT rescheduling (arrBuf=%d rtfRoom=%d "
                    "dispOK=%d proc=%d) — waiting for slotRelease\n",
                    (int)arriveBuffer.size(), (int)rtfHasRoom,
                    (int)dispatcherCanProgress, (int)processingInFlight);
        } else {
            // Either no busy state or output pending: schedule.
            // When output is pending (AHB hit at +portAhbBufferLatency),
            // schedule at the drain tick so the next dispatch
            // serializes behind it — mirrors PSM's
            // scheduleNextPopArriveBuffer with outputBuffer.front().tick.
            Tick next = curTick() + addressPhaseLatency;
            if (outputPending && outputBuffer.front().tick > next) {
                next = outputBuffer.front().tick;
            }
            DPRINTF(ARTPipeline,
                    "popArriveBuf: scheduling next at tick %d "
                    "(rtfRoom=%d dispOK=%d outBufFront=%d "
                    "dataBusy=%d outPend=%d)\n",
                    (int)next, (int)rtfHasRoom,
                    (int)dispatcherCanProgress,
                    (int)(outputBuffer.empty() ? -1
                          : outputBuffer.front().tick),
                    (int)dataPhaseBusy, (int)outputPending);
            schedule(popArriveBufferEvent, std::max(next, curTick()));
        }
    }
}

// -------------------------------------------------------------------
//  popReadyToFireBuffer (Stage 2, mirrors PSM)
//  ----------------------------------------------------------------
//  Drains one entry from readyToFireBuffer.  Order:
//
//    1. Case G: stale stream-id flush at front of readyToFireBuffer.
//    2. Defer if processingInFlight (Stage 2 single-slot lock; entry
//       stays for popOutputBuffer's reschedule).
//    3. Case A: AHB-port buffer hit — fast path, never falls through
//       to cache/prefetch.
//    4. Case B1: currentBuffer hit (pipelined; no slot, no lock).
//    5. Case B2: prefetchBuffer hit (atomically promoted; no slot).
//    6. AHB+pf+cur miss → push back to arriveBuffer front and run
//       the serialized dispatcher (Case C/D/E/F).  Stage 3 will
//       replace this with the PSM-equivalent direct flash path.
// -------------------------------------------------------------------

void
ART::popReadyToFireBuffer()
{
    // Case G: stale stream-id flush from readyToFireBuffer front.
    while (!readyToFireBuffer.empty()
           && readyToFireBuffer.front().pkt->req->hasStreamId()
           && readyToFireBuffer.front().pkt->req->streamId()
              != lastStreamId) {
        PacketPtr stale = readyToFireBuffer.front().pkt;
        DPRINTF(ARTPipeline,
                "popReadyToFire: dropping stale addr=%s streamId=%u "
                "(current=%u)\n",
                addrToString(stale->getAddr()),
                stale->req->streamId(), lastStreamId);
        lineTraceEvent("staleDrop", stale->getAddr());
        // Silent delete (mirrors PSM) — see recvTimingReq for rationale.
        readyToFireBuffer.pop_front();
        delete stale;
    }

    if (readyToFireBuffer.empty())
        return;

    // Stage 2: keep the single-slot serialization for cases C/D/E/F.
    // If a serialized request is in flight, the entry waits in
    // readyToFireBuffer until popOutputBuffer drains and clears the
    // lock (which then re-fires popArriveBuffer via
    // scheduleNextArriveBuffer).
    if (processingInFlight) {
        DPRINTF(ARTPipeline,
                "popReadyToFire: processingInFlight=true, deferring "
                "(rtf=%d) %s\n",
                (int)readyToFireBuffer.size(), slotsSnapshot().c_str());
        return;
    }

    PacketPtr pkt = readyToFireBuffer.front().pkt;

    DPRINTF(ARTPipeline,
            "popReadyToFire: dispatching addr=%s arrBuf=%d rtf=%d\n",
            addrToString(pkt->getAddr()),
            (int)arriveBuffer.size(), (int)readyToFireBuffer.size());

    // Case A: AHB-port buffer hit (port-side data-phase pipelining).
    // The AHB buffer mirrors the most recently delivered line at the
    // cpuSidePort.  Sequential within-line accesses (e.g. the second
    // 32-bit Thumb-2 instruction in an 8-byte ART line) hit here and
    // pay only one AHB data-phase, pipelining with the next request's
    // address phase.  Per RM0440 Figure 3 (3 WS, with prefetch), the
    // AHB protocol delivers consecutive within-line fetches at 1 HCLK
    // pitch — this is what models that behavior.  Case A NEVER falls
    // through to the cache/prefetch path — that is a hard requirement
    // of the refactor.
    //
    // streamIdOk: mirror PSM's portBufferedStreamId gate
    // (gem5/src/mem/pipelined_simple_mem.cc:222).  When a taken-branch
    // redirect bumps streamId in the F-stage, the real silicon's Flash
    // sense-amp is overwritten by any wrong-path prefetch past the
    // branch.  Modeling this requires the AHB buffer to miss when the
    // new request's streamId differs from the buffer's tagged streamId
    // — even if the block address still matches.  Without this gate,
    // ART's AHB buffer was hitting on stale lines from the prior
    // stream, producing ~2.5x more AHB hits than PSM (e.g. on
    // bench-bp_nested: 78606 ART hits vs 46791 PSM hits → ART ran
    // 47 % faster than PSM).  Untagged requests (DCode loads, system-
    // port functional accesses) fall back to address-only matching.
    const bool pktHasStreamId = pkt->req->hasStreamId();
    const bool ahbStreamIdOk = !pktHasStreamId ||
            pkt->req->streamId() == ahbBufferedStreamId;
    if (portAhbBufferSize > 0 && !pkt->isSecure() &&
            ahbBuffer.isHit(pkt->getAddr()) && ahbStreamIdOk) {
        DPRINTF(ARTBuffers,
                "popReadyToFire: AHB-buffer HIT addr=%s\n",
                addrToString(pkt->getAddr()));
        ++stats.currentBufferHits;  // count as a buffer hit (no flash)
        readyToFireBuffer.pop_front();
        serveFromAHBBuffer(pkt);
        lineTraceEvent("ahbBufHit", pkt->getAddr());

        // Mirror PSM's scheduleNextPopArriveBuffer(curTick +
        // bufferHitLatency): pre-emptively schedule popArrBuf at the
        // AHB-hit drain tick.  This way, when a subsequent recvTimingReq
        // arrives during the AHB hit's data-phase window, it sees
        // popArrBufEvent already scheduled and doesn't override with
        // its default +addressPhaseLatency schedule (which would let
        // ART dispatch the next miss ~5000 ticks earlier than PSM).
        Tick drainTick = curTick() + portAhbBufferLatency;
        if (!popArriveBufferEvent.scheduled()) {
            DPRINTF(ARTPipeline,
                    "popReadyToFire: pre-scheduling popArriveBuffer at "
                    "AHB-hit drain tick %d (mirror PSM's "
                    "scheduleNextPopArriveBuffer)\n",
                    (int)drainTick);
            schedule(popArriveBufferEvent,
                     std::max(drainTick, curTick()));
        }
        return;
    }
    DPRINTF(ARTBuffers,
            "popReadyToFire: AHB-buffer MISS addr=%s (size=%u valid=%d)\n",
            addrToString(pkt->getAddr()),
            portAhbBufferSize, (int)ahbBuffer.isValid());

    // Cases B1/B2: ART prefetch buffer hits (pipelined; no slot).
    // Only when prefetch is enabled and the request is non-secure.
    bool curHit = enablePrefetch && !pkt->isSecure() &&
                  currentBuffer.isHit(pkt->getAddr());
    bool pfHit = !curHit && enablePrefetch && !pkt->isSecure() &&
                 prefetchBuffer.isHit(pkt->getAddr());

    DPRINTF(ARTBuffers,
            "popReadyToFire: addr=%s curHit=%d pfHit=%d\n",
            addrToString(pkt->getAddr()), (int)curHit, (int)pfHit);

    if (curHit) {
        // Case B1: currentBuffer hit
        ++stats.currentBufferHits;
        readyToFireBuffer.pop_front();
        serveFromBuffer(pkt, currentBuffer);
        lineTraceEvent("serveCurBuf", pkt->getAddr());
        maybeIssueNextPrefetch(pkt->getAddr(), /*prefetchHit=*/true,
                               /*schedImmediate=*/true);
        return;
    }
    if (pfHit) {
        // Case B2: prefetchBuffer hit, atomically promote pf→cur BEFORE
        // serving so the next pipelined request sees the new currentBuffer.
        ++stats.prefetchBufferHits;
        currentBuffer.invalidate();
        currentBuffer.copyFrom(prefetchBuffer);
        prefetchBuffer.invalidate();
        readyToFireBuffer.pop_front();
        DPRINTF(ARTBuffers,
                "popReadyToFire: promote prefetchBuffer→currentBuffer\n");
        serveFromBuffer(pkt, currentBuffer);
        lineTraceEvent("servePfBuf", pkt->getAddr());
        maybeIssueNextPrefetch(pkt->getAddr(), /*prefetchHit=*/true,
                               /*schedImmediate=*/true);
        return;
    }

    // Cases C/D/E/F: AHB miss + buffer miss.
    //
    // Stage 3: case F (cache off + prefetch off + psm_compatible_bypass)
    // dispatches directly to flash via issueFlashRead, skipping the
    // bypassCacheEntry queue and `schedMemSideSendEvent`'s clockEdge
    // realignment.  The response is delivered at curTick() (no
    // `clockEdge(Cycles(1))` realignment).  This eliminates the
    // ~2-cycle/miss overhead vs PSM that was the root cause of the
    // art_bypass-vs-no_art divergence.  See the issue's timeline for
    // the per-miss tick-by-tick analysis.
    if (directMemoryMode && !enablePrefetch && psmCompatibleBypass) {
        // Data-phase serialization: physical flash + flash_bus accept
        // only one in-flight read at a time.  PSM models this via
        // ``nextAcceptTick``; ART models it via the inFlightSlots
        // substrate — only one slot may be in DataPhase concurrently.
        // If a slot is already in DataPhase, defer.  popOutputBuffer
        // re-fires popArriveBuffer (and thus popReadyToFireBuffer)
        // when the data-phase slot is released by recvTimingResp.
        int busySlot = slotFirstInDataPhase();
        if (busySlot >= 0) {
            DPRINTF(ARTPipeline,
                    "popReadyToFire: case F flash busy "
                    "(slot %d in DataPhase), deferring addr=%s %s\n",
                    busySlot, addrToString(pkt->getAddr()),
                    slotsSnapshot().c_str());
            return;
        }
        int slotIdx = slotAcquire(pkt, InFlightStage::DataPhase);
        if (slotIdx < 0) {
            // Should not happen given the busy check above and that
            // maxOutstandingRequests >= 1, but handle defensively.
            DPRINTF(ARTPipeline,
                    "popReadyToFire: case F slot acquire failed "
                    "(capacity=%u), deferring %s\n",
                    (unsigned)inFlightSlots.size(),
                    slotsSnapshot().c_str());
            return;
        }
        readyToFireBuffer.pop_front();
        ++stats.bypassAccesses;
        DPRINTF(ARTBypass,
                "popReadyToFire: case F PSM bypass addr=%s slot=%d "
                "(direct flash dispatch, no schedMemSideSendEvent)\n",
                addrToString(pkt->getAddr()), slotIdx);
        lineTraceEvent("caseF", pkt->getAddr());
        issueFlashRead(pkt, slotIdx);
        return;
    }

    // Cases C/D/E (cache or prefetch enabled): legacy serialized
    // fallback.  Stage 4 will plumb these through the new dispatcher
    // on the inFlightSlots substrate.
    readyToFireBuffer.pop_front();
    arriveBuffer.emplace_front(pkt, curTick());
    DPRINTF(ARTBypass,
            "popReadyToFire: AHB+pf+cur miss → serialized fallback "
            "addr=%s direct_mode=%d enable_prefetch=%d "
            "psm_compat=%d\n",
            addrToString(pkt->getAddr()),
            (int)directMemoryMode, (int)enablePrefetch,
            (int)psmCompatibleBypass);
    lineTraceEvent("serFallback", pkt->getAddr());
    popArriveBufferSerialized();
}

// -------------------------------------------------------------------
//  Stage 3: case-F PSM-equivalent direct flash dispatch.
//  ----------------------------------------------------------------
//  Mirrors PipelinedSimpleMemory::recvTimingReq's flash dispatch for
//  the case where ART has both cache and prefetch bypassed.  Skips
//  the BaseCache schedMemSideSendEvent + bypassCacheEntry indirection
//  (which forces a clockEdge realignment, costing ~1 cycle), and the
//  recvTimingResp's pushToOutputBuffer(clockEdge(Cycles(1)))
//  realignment (another ~1 cycle).  The result: PSM-equivalent
//  per-miss timing.
// -------------------------------------------------------------------

void
ART::issueFlashRead(PacketPtr pkt, int slotIdx)
{
    // PSM-equivalent flash dispatch: model the flash access internally
    // (functional read for data, scheduled internal response for timing)
    // instead of going through the standard memSidePort → flash_bus
    // XBar → SimpleMemory event machinery.  The latter accumulates
    // ~118 ticks/event of overhead that shifts interrupt-firing
    // alignment relative to PSM (which models everything inside one
    // SimObject).
    //
    // This mirrors PSM's internal flash dispatch exactly:
    //   PSM: recvTimingReq → access() (functional read) →
    //        outputBuffer.push(tick = nextAcceptTick) →
    //        popOutputBuffer(nextAcceptTick) → sendTimingResp.
    //   ART case F (after this change): same, but the functional read
    //        goes through memSidePort.sendFunctional (still hits the
    //        SimpleMemory backing through flash_bus, but functionally
    //        — no events, no timing).
    //
    // The bench's stats lose memory-side timing events for case F
    // dispatches (no flash_bus events, no SimpleMemory dequeueEvent),
    // which is correct: in case F, ART models the flash access itself.

    Addr blkAddr = convertAddrToPfBlockAddr(pkt->getAddr());
    RequestPtr req = std::make_shared<Request>(
        blkAddr, pfBlkSize, 0, pkt->req->requestorId());
    PacketPtr flashPkt = new Packet(req, MemCmd::ReadReq);
    flashPkt->allocate();

    // Functional read of flash data — bypasses XBar/event timing,
    // pure data fetch from the SimpleMemory backing.
    memSidePort.sendFunctional(flashPkt);

    // Translate the line-sized response to the CPU-sized request.
    pkt->setData(getSubBlockData(flashPkt, pkt));
    pkt->makeTimingResponse();
    pkt->headerDelay = pkt->payloadDelay = 0;

    // Schedule the response delivery at curTick + flash_latency.
    // popOutputBuffer drains at this tick and calls sendTimingResp.
    Tick responseTick = curTick() + flashResponseLatency;

    inFlightSlots[slotIdx].stage = InFlightStage::DataPhase;
    // Slot is released by popOutputBuffer when this entry drains
    // (see slotFindByPkt-based release in popOutputBuffer's unbound
    // branch).

    DPRINTF(ARTBypass,
            "issueFlashRead: case F PSM-internal dispatch addr=%s "
            "blk=%s slot=%d responseTick=%d (functional read, scheduled "
            "internally)\n",
            addrToString(pkt->getAddr()), addrToString(blkAddr),
            slotIdx, responseTick);
    lineTraceEvent("psmFlashFetch", blkAddr);

    // Carry the 8-byte line data through DeferredPacket so the AHB
    // buffer fill happens at response-delivery tick (in
    // popOutputBuffer), NOT at dispatch tick.  Mirrors PSM's
    // portBufferedBlockAddr update timing.  Filling the AHB buffer
    // here (at dispatch) would let within-line follow-up requests
    // AHB-HIT before the flash response is even delivered — making
    // ART dispatch the next miss ~5000 ticks earlier than PSM.
    uint32_t respStreamId = pkt->req->hasStreamId()
                          ? pkt->req->streamId()
                          : ~uint32_t(0);

    Tick safeReadyTick = responseTick;
    if (!outputBuffer.empty()) {
        safeReadyTick = std::max(safeReadyTick,
                                 outputBuffer.back().tick + 1);
    }
    DeferredPacket entry(pkt, safeReadyTick, /*boundToInFlight=*/false);
    entry.hasLineData = true;
    entry.lineBlockAddr = flashPkt->getAddr();
    entry.lineStreamId = respStreamId;
    entry.lineData.assign(flashPkt->getPtr<uint8_t>(),
                          flashPkt->getPtr<uint8_t>() + pfBlkSize);
    outputBuffer.emplace_back(std::move(entry));
    DPRINTF(ARTCache,
            "issueFlashRead: deferred AHB fill addr=%s data8="
            "(safeReadyTick=%d)\n",
            addrToString(flashPkt->getAddr()), safeReadyTick);
    lineTraceEvent("pushOut", pkt->getAddr(), "free");
    scheduleOutputDrain();

    delete flashPkt;
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
        DPRINTF(ARTCache, "popArriveBuffer: dropping stale pkt "
                "addr=%s\n", addrToString(stale->getAddr()));
        // Silent delete (mirrors PSM) — see recvTimingReq for rationale.
        arriveBuffer.pop_front();
        delete stale;
    }

    if (arriveBuffer.empty()) {
        maybeUnblockArriveBuffer();
        return;
    }

    // Pop front entry and mark pipeline busy
    PacketPtr pkt = arriveBuffer.front().pkt;
    arriveBuffer.pop_front();
    processingInFlight = true;
    // Stage-1 scaffolding: acquire a slot to track this in-flight
    // request.  With max_outstanding=1 this is 1:1 with
    // processingInFlight; debug-only in Stage 1.
    int acqIdx = slotAcquire(pkt, InFlightStage::DataPhase);
    DPRINTF(ARTPipeline,
            "popArriveBufferSerialized: slot[%d] acquire pkt=%s; %s\n",
            acqIdx, addrToString(pkt->getAddr()),
            slotsSnapshot().c_str());
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
    // --- Stage 3: case F PSM-equivalent bypass response ---
    // Direct path: pushToOutputBuffer at curTick() (no clockEdge
    // realignment), update AHB buffer, release slot.  This branch
    // fires when the request was dispatched via issueFlashRead.
    {
        auto *psmState = dynamic_cast<ARTPSMBypassState *>(
            pkt->findNextSenderState<ARTPSMBypassState>());
        if (psmState) {
            pkt->popSenderState();
            PacketPtr cpuPkt = psmState->cpuPkt;
            int slotIdx = psmState->slotIdx;
            DPRINTF(ARTBypass,
                    "recvTimingResp: case F response addr=%s slot=%d\n",
                    addrToString(cpuPkt->getAddr()), slotIdx);
            lineTraceEvent("caseFResp", cpuPkt->getAddr());

            // Translate the line-sized flash response to the original
            // CPU-sized packet.
            cpuPkt->setData(getSubBlockData(pkt, cpuPkt));
            cpuPkt->makeTimingResponse();
            cpuPkt->headerDelay = cpuPkt->payloadDelay = 0;

            // Push at curTick() — NO clockEdge realignment.  This is
            // the half of Stage 3 that closes the response-side cycle
            // (the dispatch-side cycle is closed by issueFlashRead's
            // direct sendTimingReq).
            pushToOutputBuffer(cpuPkt, curTick(),
                               /*boundToInFlight=*/false);

            // Mirror line into AHB buffer (case I in the design table).
            // Tag with the CPU packet's streamId — that's the consumer
            // that will see this AHB-buffer entry first.  Subsequent
            // requests with the same streamId hit; new-stream requests
            // miss (matching PSM's portBufferedStreamId behaviour).
            uint32_t respStreamId = cpuPkt->req->hasStreamId()
                                  ? cpuPkt->req->streamId()
                                  : ~uint32_t(0);
            updateAHBBufferFromRaw(pkt->getAddr(),
                                   pkt->getPtr<uint8_t>(),
                                   respStreamId);

            // Release the slot.  popOutputBuffer's drain will fire
            // scheduleNextArriveBuffer because boundToInFlight=false
            // and processingInFlight stays false in case F.
            slotRelease(slotIdx);

            delete psmState;
            delete pkt;
            return;
        }
    }

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
                // Mirror the bypass-delivered line into the AHB buffer.
                updateAHBBufferFromRaw(pkt->getAddr(),
                                       pkt->getPtr<uint8_t>());
                state->cpuPkt = nullptr;
                delete state;
                cachePktEntry = nullptr;
                delete pkt;
            } else {
                // No translate state — send response as-is
                pushToOutputBuffer(pkt, clockEdge(Cycles(1)),
                                   /*boundToInFlight=*/true);
                updateAHBBufferFromRaw(pkt->getAddr(),
                                       pkt->getPtr<uint8_t>());
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
            // The line just landed at the AHB port — mirror it into
            // the AHB buffer so the next within-line CPU access can
            // pipeline through the AHB data-phase path.
            updateAHBBufferFromRaw(entry->blkAddr,
                                   pkt->getPtr<uint8_t>());

            if (entry->ifCPUWaiting()) {
                DPRINTF(ARTCache,
                        "recvTimingResp: serving waiting CPU\n");
                entry->cpuPtr->setData(
                    getSubBlockData(pkt, entry->cpuPtr));
                entry->cpuPtr->makeTimingResponse();
                Tick readyTick = std::max(
                    curTick() + prefetchBufferHitLatency + 1,
                    clockEdge(Cycles(0)));
                // CPU was waiting for this in-flight prefetch (case 4).
                // Always serialized — mark response bound.  Only
                // reachable when enablePrefetch=true (artPfEntry only
                // exists in that mode) — bypass mode never lands here.
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
    // Mirror the cache-hit line into the AHB buffer so subsequent
    // within-line accesses can pipeline through the AHB data-phase.
    updateAHBBufferFromRaw(pkt->getAddr(), pkt->getPtr<uint8_t>());

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
            // Mirror the MSHR-completed line into the AHB buffer so
            // subsequent within-line accesses can pipeline through
            // the AHB data-phase path.
            if (pkt && pkt->getPtr<uint8_t>())
                updateAHBBufferFromRaw(pkt->getAddr(),
                                       pkt->getPtr<uint8_t>());

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
