# mem/cache/ — Cache Hierarchy Context

> **Purpose:** This directory implements gem5's classic cache hierarchy including the cache itself, replacement policies, tag structures, prefetchers, and cache compressors.

## Key Files

| File | Class | Description |
|------|-------|-------------|
| `base.hh/cc` | `BaseCache` | Base cache class (`ClockedObject`). Handles coherence protocol, MSHR management, port interface. |
| `cache.hh/cc` | `Cache` | Coherent cache implementation. Supports snooping, write-back, write-allocate. |
| `noncoherent_cache.hh/cc` | `NoncoherentCache` | Cache without coherence (for I/O or private hierarchies). |
| `cache_blk.hh/cc` | `CacheBlk` | Cache block/line data structure with tags, status, data. |
| `mshr.hh/cc` | `MSHR` | Miss Status Holding Register — tracks outstanding cache misses. |
| `mshr_queue.hh/cc` | `MSHRQueue` | Queue of MSHRs with management. |
| `write_queue.hh/cc` | `WriteQueue` | Queue for writebacks and clean evictions. |
| `write_queue_entry.hh/cc` | `WriteQueueEntry` | Entry in write queue. |
| `queue.hh` | `Queue<Entry>` | Templated base for MSHR/write queues. |
| `cache_probe_arg.hh` | `CacheAccessProbeArg` | Probe argument for cache events. |

## Subdirectories

| Directory | Description | Context file |
|-----------|-------------|--------------|
| `tags/` | Tag store implementations (set-associative, FA, sector, compressed). | See below. |
| `replacement_policies/` | LRU, FIFO, LFU, Random, Tree-PLRU, BRRIP, SHIP, etc. | See below. |
| `prefetch/` | Hardware prefetcher implementations. | See below. |
| `compressors/` | Cache line compression algorithms. | See below. |

## Cache Architecture

```
          ResponsePort (cpu_side)
               │
               ▼
┌──────────────────────────────┐
│          BaseCache           │
│  ┌────────┐  ┌───────────┐  │
│  │  Tags  │  │ MSHRQueue │  │
│  │(blocks)│  │(misses)   │  │
│  └────────┘  └───────────┘  │
│  ┌────────────────────────┐  │
│  │     WriteQueue         │  │
│  │   (writebacks)         │  │
│  └────────────────────────┘  │
│  ┌────────┐  ┌────────────┐  │
│  │Prefetch│  │ Compressor │  │
│  └────────┘  └────────────┘  │
└──────────────────────────────┘
               │
               ▼
          RequestPort (mem_side)
```

## tags/ — Tag Stores

| File | Class | Description |
|------|-------|-------------|
| `base.hh/cc` | `BaseTags` | Abstract base for tag stores. |
| `base_set_assoc.hh/cc` | `BaseSetAssoc` | Set-associative tag store (most common). |
| `fa_lru.hh/cc` | `FALRU` | Fully-associative LRU (for stack-distance study). |
| `sector_tags.hh/cc` | `SectorTags` | Sector cache tags (multiple sub-blocks per tag). |
| `compressed_tags.hh/cc` | `CompressedTags` | Tags for compressed caches (variable block sizes). |
| `dueling.hh/cc` | `DuelingTags` | Set dueling between two policies. |
| `indexing_policies/` | | Index computation: set-associative, skewed-associative. |
| `partitioning_policies/` | | Cache partitioning (way partitioning, etc.). |

## replacement_policies/ — Replacement Policies

All inherit from `replacement_policy::Base`:

| File | Class | Description |
|------|-------|-------------|
| `lru_rp.hh` | `LRU` | Least Recently Used |
| `fifo_rp.hh` | `FIFO` | First In First Out |
| `lfu_rp.hh` | `LFU` | Least Frequently Used |
| `mru_rp.hh` | `MRU` | Most Recently Used |
| `random_rp.hh` | `Random` | Random replacement |
| `brrip_rp.hh` | `BRRIP` | Bimodal Re-Reference Interval Prediction |
| `bip_rp.hh` | `BIP` | Bimodal Insertion Policy |
| `ship_rp.hh` | `SHiP` | Signature-based Hit Predictor |
| `tree_plru_rp.hh` | `TreePLRU` | Tree Pseudo-LRU |
| `second_chance_rp.hh` | `SecondChance` | Second chance (clock algorithm) |
| `dueling_rp.hh` | `Dueling` | Dueling between two policies |
| `weighted_lru_rp.hh` | `WeightedLRU` | Weighted LRU |

## prefetch/ — Hardware Prefetchers

All inherit from `prefetch::Base`:

| File | Class | Description |
|------|-------|-------------|
| `base.hh/cc` | `Base` | Prefetcher base class. |
| `queued.hh/cc` | `Queued` | Base for prefetchers that queue requests. |
| `stride.hh/cc` | `StridePrefetcher` | Stride-based prefetcher. |
| `tagged.hh/cc` | `Tagged` | Tagged prefetcher (next-line). |
| `bop.hh/cc` | `BOP` | Best Offset Prefetcher. |
| `access_map_pattern_matching.hh/cc` | `AMPM` | Access Map Pattern Matching. |
| `signature_path.hh/cc` | `SignaturePath` | Signature Path Prefetcher (SPP). |
| `signature_path_v2.hh/cc` | `SignaturePathV2` | SPP v2. |
| `delta_correlating_prediction_tables.hh/cc` | `DCPT` | Delta Correlating Prediction Tables. |
| `irregular_stream_buffer.hh/cc` | `IrregularStreamBuffer` | ISB prefetcher. |
| `pif.hh/cc` | `PIF` | Prefetch-Informed Filtering. |
| `multi.hh/cc` | `Multi` | Combines multiple prefetchers. |
| `sms.hh/cc` | `SMS` | Spatial Memory Streaming. |
| `spatio_temporal_memory_streaming.hh/cc` | `STeMS` | Spatio-temporal memory streaming. |
| `slim_ampm.hh/cc` | `SlimAMPM` | Slim AMPM variant. |
| `sbooe.hh/cc` | `SBOOE` | Sandbox-Based Optimal Offset Estimation. |
| `fdp.hh/cc` | `FDP` | Feedback-Directed Prefetcher. |

## compressors/ — Cache Compression

All inherit from `compression::Base`:

| File | Class | Description |
|------|-------|-------------|
| `base.hh/cc` | `Base` | Compressor base. |
| `zero.hh/cc` | `Zero` | Zero-value compression. |
| `repeated_qwords.hh/cc` | `RepeatedQwords` | Repeated quadword compression. |
| `base_delta.hh/cc` | `BaseDelta` | Base-delta compression (BDI). |
| `cpack.hh/cc` | `CPack` | C-Pack compression. |
| `fpc.hh/cc` | `FPC` | Frequent Pattern Compression. |
| `fpcd.hh/cc` | `FPCD` | FPC with Delta. |
| `frequent_values.hh/cc` | `FrequentValues` | Frequent value compression. |
| `perfect.hh/cc` | `Perfect` | Ideal compression (for study). |
| `multi.hh/cc` | `Multi` | Combines multiple compressors. |

## Adding a New Component

### New Replacement Policy
1. Inherit from `replacement_policy::Base`
2. Implement `invalidate()`, `touch()`, `reset()`, `getVictim()`
3. Add to `ReplacementPolicies.py` and `SConscript`

### New Prefetcher
1. Inherit from `prefetch::Queued` (or `Base`)
2. Implement `calculatePrefetch()` (returns list of prefetch addresses)
3. Add to `Prefetcher.py` and `SConscript`

### New Compressor
1. Inherit from `compression::Base`
2. Implement `compress()` and `decompress()`
3. Add to `Compressors.py` and `SConscript`
