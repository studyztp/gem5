# ART Accelerator Cache Tests

Hardware-fidelity tests for the gem5 ART (Adaptive Real-Time) accelerator
cache model (`src/mem/cache/art.cc`).  The ART accelerator is modelled after
the STM32 ART instruction accelerator found in STM32F2/F4/F7/G4/H7
microcontrollers, which sits between the Cortex-M CPU and flash memory to
hide flash wait-state latency through a two-buffer sequential prefetch
scheme and an instruction cache.

## Architecture overview

The real STM32 ART accelerator contains three levels of instruction
storage:

```
CPU  -->  Current Buffer  -->  Prefetch Buffer  -->  I-Cache  -->  Flash
```

1. **Current Buffer** -- holds the most recently consumed prefetch line,
   available for immediate zero-wait-state hits.
2. **Prefetch Buffer** -- holds the line being (or just) prefetched from
   flash.  When consumed, it is promoted to the current buffer.
3. **I-Cache** -- a 1 KiB sector-tagged instruction cache (32 lines of
   4 x 64-bit sectors in dual-bank mode).  Catches re-accesses to
   previously fetched code (branches, loops larger than the buffers).

In gem5, the ART is implemented as a `NoncoherentCache` subclass.  The
current buffer, prefetch buffer, and prefetch queue entry are managed
directly in `ART::recvTimingReq` / `ART::recvTimingResp`.  The underlying
`NoncoherentCache` tag store acts as the I-Cache.

## How the tests prove correctness

Each test validates a specific hardware behaviour documented in the STM32
reference manuals (PM0059, RM0090, RM0440) by comparing gem5 simulation
statistics against a pure-Python reference model that independently
computes the exact expected values.

### Deterministic, isolated test system

The test system is intentionally minimal to make every stat deterministic
and computable from first principles:

- **TrafficGen** produces a fully deterministic address sequence (no
  randomness, no CPU pipeline effects).
- **IOXBar** with zero latency (crossbar contributes no timing).
- **SimpleMemory** with 4 ns latency at 1 GHz = 4 cycles, matching the
  STM32 flash wait states.
- **20-cycle request period** ensures every prefetch (including follow-up
  chains) completes before the next CPU request arrives.

Under this configuration, the ART's logic is fully isolated: every cycle
of timing is attributable to the ART prefetcher and flash latency alone.

### Reference model (`art_reference_model.py`)

The reference model is a standalone Python implementation of the ART
state machine that mirrors `art.cc` line-by-line:

- `ARTModel.process_request(addr)` replicates `ART::recvTimingReq`.
- `ARTModel._send_prefetch()` replicates `ART::sendARTPrefetchPacket`.
- `ARTModel._complete_prefetch()` replicates `ART::recvTimingResp` for
  prefetch responses.
- `PrefetchBuffer` and `PfEntry` replicate the C++ `ARTPrefetchBuffer`
  and `ARTPfQueueEntry` classes.

Given the same address sequence and parameters, the reference model
produces the exact same statistics as the gem5 simulation.  If any
discrepancy arises, it indicates a bug in either the gem5 model or the
reference model, which can be diagnosed using the `--trace` flag on both
to compare per-request state transitions side by side.

The reference model also tracks the I-Cache tag store (`cache_tags`) to
correctly model the interaction between the prefetch mechanism and the
underlying `NoncoherentCache` -- specifically, whether an I-Cache hit
(no MSHR, no flash read) causes the allocated prefetch to be issued or
discarded.

### Stat-by-stat verification

Every test compares all 8 ART statistics between gem5 and the reference
model.  All must match exactly:

| Statistic | Meaning |
|---|---|
| `currentBufferHits` | Requests served from the current buffer |
| `prefetchBufferHits` | Requests served from the prefetch buffer |
| `prefetchMisses` | Requests not served by either buffer |
| `bypassAccesses` | Requests forwarded directly to memory (bypass mode) |
| `cpuWaitEvents` | Times the CPU waited for an in-flight prefetch |
| `prefetchesAllocated` | Total prefetch entries allocated (includes discarded) |
| `prefetchesIssued` | Total prefetch requests actually sent to memory |
| `prefetchesCompleted` | Total prefetch responses received from memory |

## Test scenarios

### 1. `hw_sequential_prefetch`

**Hardware behaviour**: PM0059 Section 2.4.2 sequential prefetch pipeline.

Sequential instruction fetches from a 4 KiB address range.  After the
initial cold miss, the prefetcher keeps the pipeline full: each request
hits the current buffer while the next line is being prefetched.
Verifies the steady-state prefetch hit rate.

### 2. `hw_branch_penalty`

**Hardware behaviour**: non-sequential access defeats prefetch.

Each request jumps to a different address (16 branch targets, cycling).
The prefetcher allocates a next-sequential prefetch every time, but the
next request is never sequential, so the prefetch is always discarded.
Verifies that non-sequential access patterns result in zero buffer hits.

### 3. `hw_buffer_promotion`

**Hardware behaviour**: two-buffer promotion mechanism.

Sequential fetches over a smaller 2 KiB range that wraps around.  At the
wrap point, the address jumps back to 0, which is already in the I-Cache.
This tests the promotion of prefetch buffer to current buffer and the
interaction between wrap-around misses and the prefetch pipeline.

### 4. `hw_prefetch_disable`

**Hardware behaviour**: PRFTEN=0 disables all prefetch activity.

All ART prefetch statistics must be zero.  Requests pass through to the
`NoncoherentCache` normally.

### 5. `hw_direct_memory_bypass`

**Hardware behaviour**: direct flash access path (no ART).

Models the zero-wait-state flash access mode where every request bypasses
the cache and goes straight to memory.  Verifies that `bypassAccesses`
equals the total request count and all prefetch stats are zero.

### 6. `hw_flash_range_bounds`

**Hardware behaviour**: prefetch limited to flash address region.

The flash region is configured as `[0x0000, 0x00FF]`, but the traffic
pattern spans `[0, 1024)`.  Prefetches are only issued for addresses
within the flash region.  Verifies correct bounds checking.

### 7. `hw_completion_fidelity`

**Hardware behaviour**: prefetch lifecycle state machine check.

Sequential fetches over a small 512-byte range with shorter duration.
Tests the full prefetch lifecycle (allocate, issue, complete, promote)
and verifies the relationship between allocated, issued, and completed
counts.

## Configurable I-Cache hit behaviour (`prefetch_on_cache_hit`)

When a request misses both ART buffers but hits in the I-Cache (the
underlying `NoncoherentCache`), no MSHR is created and no flash read
occurs.  The question is: should the just-allocated prefetch be issued
immediately, or left in `ALLOC` state to be discarded by the next
request?

The STM32 documentation does not specify this behaviour, so the gem5
model provides a configurable parameter:

- **`prefetch_on_cache_hit=False`** (default): the prefetch stays in
  `ALLOC` state and is discarded when the next request arrives.  This
  is the conservative behaviour that avoids potentially misaligned
  flash fetches.

- **`prefetch_on_cache_hit=True`**: the prefetch is immediately
  scheduled via `schedMemSideSendEvent`, causing a flash read for the
  next sequential line.  This is the eager behaviour that maximises
  prefetch coverage.

Each prefetch-enabled test is registered in both variants (`_no_pf_on_hit`
and `_pf_on_hit`) to ensure both modes are validated.

## Running the tests

### Through the gem5 test framework

```bash
./build/ALL/gem5.opt -m pytest tests/gem5/art_cache/
```

Or as part of the full daily test suite:

```bash
./build/ALL/gem5.opt -m pytest tests/gem5/ -k "art_cache"
```

### Manually (single test)

```bash
# Conservative mode (default)
./build/ALL/gem5.opt tests/gem5/art_cache/configs/art_test_run.py \
    --test-type hw_sequential_prefetch

# Eager prefetch-on-cache-hit mode
./build/ALL/gem5.opt tests/gem5/art_cache/configs/art_test_run.py \
    --test-type hw_sequential_prefetch --prefetch-on-cache-hit

# With debug traces for both gem5 and the reference model
./build/ALL/gem5.opt tests/gem5/art_cache/configs/art_test_run.py \
    --test-type hw_sequential_prefetch --trace
python3 tests/gem5/art_cache/configs/art_reference_model.py \
    hw_sequential_prefetch --trace
```

### Reference model standalone

```bash
# Compute expected stats for all test types
python3 tests/gem5/art_cache/configs/art_reference_model.py

# With trace output and eager prefetch mode
python3 tests/gem5/art_cache/configs/art_reference_model.py \
    --trace --prefetch-on-cache-hit

# Single test type
python3 tests/gem5/art_cache/configs/art_reference_model.py \
    hw_buffer_promotion --trace
```

## File structure

```
tests/gem5/art_cache/
    test_art_cache.py           # Test registration (gem5 test framework)
    README.md                   # This file
    configs/
        art_test_run.py         # gem5 simulation config (TrafficGen + ART)
        art_reference_model.py  # Pure-Python reference model
```

## References

- **PM0059** -- STM32F205/215, STM32F207/217 Flash programming manual,
  Section 2.4.2 (ART Accelerator)
- **RM0090** -- STM32F405/415, STM32F407/417, STM32F427/437, STM32F429/439
  Reference manual, Section 3.4 (Adaptive real-time memory accelerator)
- **RM0440** -- STM32G4 Series Reference manual, Section 3.3.5
  (Instruction cache, data cache, and prefetch buffer)
- **STM32H7 ART Product Training** --
  STM32H7-System-Adaptive\_Real-Time\_Accelerator\_ART.pdf
