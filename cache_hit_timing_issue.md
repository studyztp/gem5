# ART Cache Hit Timing Issue: `curTick()+1` vs `clockEdge(0)`

## Problem

Changing `handleTimingReqHit` to return at `curTick()+1` instead of `request_time` (= `clockEdge(0)`) causes a segfault in `Fetch2::decodeInstructions` at `fetch2.cc:410`.

## Root Cause

With `curTick()+1`, the cache hit response arrives **mid-cycle** before MinorCPU's `SingleStageFetch1::evaluate()` has advanced the PC for this fetch line. The stale PC from the previous decode iteration is used to compute `inputIndex`, causing an underflow.

### GDB Evidence

```
fetch_info.inputIndex = 4294967292    (= 0xFFFFFFFC = -4 unsigned)
line_in->lineWidth    = 4
line_in->lineBaseAddr = 0x8000278
line_in->fetchAddr    = 0x8000278
line_in->pc->_pc      = 0x8000276     ← PC is BEFORE lineBaseAddr!
fetch_info.pc->_pc    = 0x8000276     ← same stale PC
```

- **PC = `0x8000276`** (stale, from previous decode)
- **lineBaseAddr = `0x8000278`** (correct, from this fetch)
- **inputIndex = `0x8000276 - 0x8000278` = -2** (wraps to huge unsigned)
- `memcpy` reads past buffer → segfault

### Why `clockEdge(0)` works

`clockEdge(0)` returns the **next clock edge** (≥ `curTick()`). Since `popArriveBuffer` fires mid-cycle (after `addressPhaseLatency`), `clockEdge(0)` is always the next cycle boundary. By the time `evaluate()` runs at the next clock edge, the PC has been properly updated.

### Why `curTick()+1` fails

`curTick()+1` is still mid-cycle. The response is delivered before the next `evaluate()` call properly initializes the PC for this line. The `wakeupOnEventImmediate()` path in `SingleStageFetch1::recvTimingResp` schedules `evaluate()` at `clockEdge(0)` anyway (see `Pipeline::startThisCycle()` at `pipeline.cc:303`), so the response arrives but sits until the next clock edge. However, the PC state in `fetchInfo` hasn't been updated to match the new line.

## Impact

This means **I-Cache hits always cost 1 cycle** in the MinorCPU model, regardless of the ART's `tag_latency`/`data_latency` settings. The response cannot be delivered faster than `clockEdge(0)` without corrupting MinorCPU's PC tracking.

### Cycle comparison

| Path | Response timing | Effective latency |
|------|----------------|-------------------|
| Buffer hit (curBuf/pfBuf) | `curTick() + bufferHitLatency + 1` | ~0 cycles (mid-cycle) |
| Wait for prefetch | `curTick() + bufferHitLatency + 1` | ~0 cycles (mid-cycle) |
| I-Cache hit | `clockEdge(0)` = next clock edge | **1 cycle** |
| I-Cache miss (MSHR) | `clockEdge(responseLatency)` | 1+ cycles |

Buffer hits and prefetch waits work at `curTick()+1` because they bypass the `NoncoherentCache::recvTimingReq` → `BaseCache::access()` pipeline. The cache hit path goes through `access()` which sets `request_time = clockEdge(lat)`, and the PC tracking in Fetch2 depends on this cycle-aligned timing.

## Possible Fixes

1. **Accept 1-cycle cache hit latency** — matches MinorCPU's pipeline model where every memory response takes at least 1 cycle.

2. **Serve I-Cache hits directly in `popArriveBuffer`** (like buffer hits) — read from tag store, bypass BaseCache pipeline. Previous attempts crashed because the packet state was wrong. Would need to construct a proper response packet matching what `adoptPacketData()` expects.

3. **Modify MinorCPU's `SingleStageFetch1::evaluate()`** to handle same-cycle responses — update PC tracking before calling `runDecodeCore()` when a new line arrives in the same cycle.

## Configuration

- CPU: `SingleStageFetch1` with `fetch1LineWidth=4`, `fetch1LineSnapWidth=4`
- ART: `pfBlkSize=8`, `cache_blk_size=8`
- Clock: 170 MHz (5882 ticks/cycle)
- The 4→8 byte translation in ART returns 4-byte CPU packets to MinorCPU
