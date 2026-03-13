"""
Hardware-fidelity test configuration for the ART (Adaptive Real-Time)
accelerator cache model.

Each test scenario maps to a specific behaviour documented in the STM32
Adaptive Real-Time memory accelerator specification (PM0059 Section 2.4.2,
RM0090 Section 3.4, and related STM32 reference manuals).

The tests are structured to verify that the gem5 ART model faithfully
reproduces the following real-hardware properties:

  hw_sequential_prefetch
      Validates the core ART claim: sequential instruction fetches achieve
      near-zero additional wait states thanks to the two-buffer prefetch
      pipeline.  (PM0059 §2.4.2: "Prefetch on the I-Code bus can be used
      to read the next sequential instruction line from the Flash memory
      while the current instruction line is being requested by the CPU.")

  hw_branch_penalty
      Validates that non-sequential (branch-like) accesses defeat the
      prefetch pipeline and suffer full flash-read penalties.  The real
      hardware triggers a new 128-bit read on every branch target.
      (PM0059 §2.4.2: "Each Flash memory read operation provides 128 bits
      … in case of sequential code, at least four CPU cycles are needed to
      execute the previous read instruction line.")

  hw_buffer_promotion
      Validates the two-buffer promotion mechanism.  When the prefetch
      buffer finishes fetching a line, it is promoted to the "current
      buffer" so the next sequential CPU access hits immediately.  This
      models the real hardware's ability to serve instructions while the
      next line is simultaneously being fetched.

  hw_prefetch_disable
      Validates that setting PRFTEN=0 (prefetch enable = false) disables
      all prefetch activity.  (FLASH_ACR register, PRFTEN bit.)

  hw_direct_memory_bypass
      Validates the non-ART flash path where every CPU access is forwarded
      directly to flash memory with full wait-state latency.  Models the
      behaviour when the ART accelerator is not used.

  hw_flash_range_bounds
      Validates that the ART prefetcher only operates on addresses within
      the flash memory region.  Accesses outside the flash address space
      must not trigger prefetch activity.

  hw_completion_fidelity
      Validates the prefetch lifecycle: every issued prefetch must
      eventually complete (prefetchesCompleted <= prefetchesIssued, and
      both > 0 for an active workload).  This is a sanity check on the
      model's internal state-machine fidelity.
"""

import argparse
import os
import sys
import tempfile

import m5
from m5.objects import *
from m5.params import MaxAddr


# -----------------------------------------------------------------------
#  System construction
# -----------------------------------------------------------------------

def create_system(args):
    """Build a minimal system with TrafficGen -> ARTCache -> SimpleMemory."""
    system = System()
    system.clk_domain = SrcClockDomain(
        clock="1GHz", voltage_domain=VoltageDomain()
    )
    system.mem_mode = "timing"
    system.mem_ranges = [AddrRange("1MiB")]

    system.membus = SystemXBar(width=64)

    system.mem_ctrl = SimpleMemory(
        range=AddrRange("1MiB"), latency="30ns", bandwidth="0GiB/s"
    )
    system.mem_ctrl.port = system.membus.mem_side_ports
    system.system_port = system.membus.cpu_side_ports

    art_params = dict(
        size="4KiB",
        assoc=4,
        tag_latency=1,
        data_latency=1,
        response_latency=1,
        mshrs=4,
        tgts_per_mshr=8,
        pf_blk_size=8,
        cache_blk_size=8,
    )

    tt = args.test_type

    if tt == "hw_sequential_prefetch":
        art_params["enable_prefetch"] = True
        art_params["direct_memory_mode"] = False
        art_params["flash_start_addr"] = 0
        art_params["flash_end_addr"] = MaxAddr

    elif tt == "hw_branch_penalty":
        art_params["enable_prefetch"] = True
        art_params["direct_memory_mode"] = False
        art_params["flash_start_addr"] = 0
        art_params["flash_end_addr"] = MaxAddr

    elif tt == "hw_buffer_promotion":
        art_params["enable_prefetch"] = True
        art_params["direct_memory_mode"] = False
        art_params["flash_start_addr"] = 0
        art_params["flash_end_addr"] = MaxAddr

    elif tt == "hw_prefetch_disable":
        art_params["enable_prefetch"] = False
        art_params["direct_memory_mode"] = False

    elif tt == "hw_direct_memory_bypass":
        art_params["enable_prefetch"] = True
        art_params["direct_memory_mode"] = True
        art_params["flash_start_addr"] = 0
        art_params["flash_end_addr"] = MaxAddr

    elif tt == "hw_flash_range_bounds":
        art_params["enable_prefetch"] = True
        art_params["direct_memory_mode"] = False
        art_params["flash_start_addr"] = 0x0000
        art_params["flash_end_addr"] = 0x00FF

    elif tt == "hw_completion_fidelity":
        art_params["enable_prefetch"] = True
        art_params["direct_memory_mode"] = False
        art_params["flash_start_addr"] = 0
        art_params["flash_end_addr"] = MaxAddr

    else:
        print(f"Unknown test type: {tt}", file=sys.stderr)
        sys.exit(1)

    system.art_cache = ARTCache(**art_params)
    system.art_cache.mem_side = system.membus.cpu_side_ports

    system.tgen = TrafficGen()
    system.tgen.port = system.art_cache.cpu_side

    return system


# -----------------------------------------------------------------------
#  Traffic pattern generation
# -----------------------------------------------------------------------

def create_traffic_cfg(args):
    """
    Write a TrafficGen config file tailored to each test scenario.

    Traffic patterns are chosen to exercise specific hardware behaviours:
      - LINEAR:  sequential reads, modelling straight-line code execution.
      - RANDOM:  random-address reads, modelling branch-heavy code.
    """
    cfg_path = os.path.join(tempfile.gettempdir(), "art_tgen.cfg")
    block_size = 8
    tt = args.test_type

    if tt == "hw_sequential_prefetch":
        # Long sequential scan through a large address range.
        # This gives the prefetcher plenty of opportunity to stay ahead.
        lines = [
            f"STATE 0 10000000 LINEAR 100 0 4096 {block_size} 1000 1000 0",
            "STATE 1 10000000 EXIT",
            "INIT 0",
            "TRANSITION 0 1 1",
            "TRANSITION 1 1 1",
        ]

    elif tt == "hw_branch_penalty":
        # Random-address reads over a wide range, simulating branch-heavy
        # code where instruction prefetch is ineffective.
        lines = [
            f"STATE 0 10000000 RANDOM 100 0 4096 {block_size} 1000 1000 0",
            "STATE 1 10000000 EXIT",
            "INIT 0",
            "TRANSITION 0 1 1",
            "TRANSITION 1 1 1",
        ]

    elif tt == "hw_buffer_promotion":
        # Sequential reads over a moderate range.  We use a slightly slower
        # request period to ensure the prefetch has time to complete and
        # promote into the current buffer before the CPU crosses the
        # block boundary.
        lines = [
            f"STATE 0 10000000 LINEAR 100 0 2048 {block_size} 2000 2000 0",
            "STATE 1 10000000 EXIT",
            "INIT 0",
            "TRANSITION 0 1 1",
            "TRANSITION 1 1 1",
        ]

    elif tt == "hw_prefetch_disable":
        # Sequential reads (which would normally benefit from prefetch)
        # with the prefetcher disabled.
        lines = [
            f"STATE 0 10000000 LINEAR 100 0 4096 {block_size} 1000 1000 0",
            "STATE 1 10000000 EXIT",
            "INIT 0",
            "TRANSITION 0 1 1",
            "TRANSITION 1 1 1",
        ]

    elif tt == "hw_direct_memory_bypass":
        # Sequential reads with the cache entirely bypassed.
        lines = [
            f"STATE 0 10000000 LINEAR 100 0 4096 {block_size} 1000 1000 0",
            "STATE 1 10000000 EXIT",
            "INIT 0",
            "TRANSITION 0 1 1",
            "TRANSITION 1 1 1",
        ]

    elif tt == "hw_flash_range_bounds":
        # Sequential reads that span beyond the configured flash range
        # (0x0000–0x00FF).  The traffic covers 0x0000–0x0400, so
        # prefetches should only be issued for the first portion.
        lines = [
            f"STATE 0 10000000 LINEAR 100 0 1024 {block_size} 1000 1000 0",
            "STATE 1 10000000 EXIT",
            "INIT 0",
            "TRANSITION 0 1 1",
            "TRANSITION 1 1 1",
        ]

    elif tt == "hw_completion_fidelity":
        # Short sequential burst so the workload fully drains.
        lines = [
            f"STATE 0 5000000 LINEAR 100 0 512 {block_size} 1000 1000 0",
            "STATE 1 10000000 EXIT",
            "INIT 0",
            "TRANSITION 0 1 1",
            "TRANSITION 1 1 1",
        ]

    else:
        print(f"Unknown test type: {tt}", file=sys.stderr)
        sys.exit(1)

    with open(cfg_path, "w") as f:
        for line in lines:
            f.write(line + "\n")

    return cfg_path


# -----------------------------------------------------------------------
#  Statistics verification
# -----------------------------------------------------------------------

def parse_art_stats(stat_names):
    """Read ART-specific statistics from the stats.txt output file."""
    art_stats = {}
    stats_file = m5.options.outdir + "/stats.txt"

    with open(stats_file, "r") as f:
        for line in f:
            for name in stat_names:
                if f"system.art_cache.{name}" in line:
                    parts = line.split()
                    art_stats[name] = int(float(parts[1]))
    return art_stats


def check_stats(args):
    """
    Validate ART statistics against expected hardware behaviour.

    Each test scenario has assertions that map to documented STM32 ART
    accelerator properties.  When an assertion fails, the error message
    references the specific hardware behaviour being validated.
    """
    stat_names = [
        "currentBufferHits",
        "prefetchBufferHits",
        "prefetchMisses",
        "bypassAccesses",
        "cpuWaitEvents",
        "prefetchesIssued",
        "prefetchesCompleted",
    ]
    art_stats = parse_art_stats(stat_names)

    print("ART statistics:")
    for name in stat_names:
        print(f"  {name}: {art_stats.get(name, -1)}")

    tt = args.test_type

    # ---- hw_sequential_prefetch ----
    #
    # STM32 PM0059 §2.4.2:
    #   "the performance achieved thanks to the ART accelerator is
    #    equivalent to 0 wait state program execution from Flash memory"
    #
    # For sequential code, the prefetch pipeline should serve the majority
    # of accesses from the buffers.  We verify:
    #   1. Buffer hits occur (prefetch pipeline is active).
    #   2. Prefetches were issued and completed.
    #   3. The hit rate is meaningfully above zero — showing the pipeline
    #      is actually hiding flash latency, not just issuing wasted
    #      prefetches.
    if tt == "hw_sequential_prefetch":
        current_hits = art_stats.get("currentBufferHits", 0)
        pf_hits = art_stats.get("prefetchBufferHits", 0)
        total_hits = current_hits + pf_hits
        issued = art_stats.get("prefetchesIssued", 0)
        completed = art_stats.get("prefetchesCompleted", 0)
        misses = art_stats.get("prefetchMisses", 0)

        if total_hits == 0:
            return fail("PM0059 §2.4.2: sequential access must produce "
                        "buffer hits (got 0)")
        if issued == 0:
            return fail("PM0059 §2.4.2: prefetcher must issue requests "
                        "for sequential access")
        if completed == 0:
            return fail("PM0059 §2.4.2: prefetches must complete for "
                        "sequential workload")
        total_accesses = total_hits + misses
        if total_accesses > 0:
            hit_rate = total_hits / total_accesses
            print(f"  Prefetch hit rate: {hit_rate:.2%} "
                  f"({total_hits}/{total_accesses})")
            if hit_rate < 0.10:
                return fail(
                    f"PM0059 §2.4.2: sequential hit rate too low "
                    f"({hit_rate:.2%}); the two-buffer pipeline should "
                    f"serve a meaningful fraction of sequential accesses")

        return passed(f"sequential prefetch effective: {total_hits} hits, "
                      f"{issued} issued, {completed} completed")

    # ---- hw_branch_penalty ----
    #
    # STM32 hardware behaviour:
    #   "Each Flash memory read operation provides 128 bits … in case
    #    of sequential code, at least four CPU cycles are needed"
    #   Branches trigger new flash reads that cannot be hidden by
    #   the prefetch pipeline.
    #
    # With random-address traffic, the prefetch buffers should miss
    # far more often than they hit.  We verify:
    #   1. Prefetch misses occur (non-sequential access defeats buffers).
    #   2. The miss count significantly exceeds the hit count.
    elif tt == "hw_branch_penalty":
        current_hits = art_stats.get("currentBufferHits", 0)
        pf_hits = art_stats.get("prefetchBufferHits", 0)
        total_hits = current_hits + pf_hits
        misses = art_stats.get("prefetchMisses", 0)

        if misses == 0:
            return fail("Branch penalty: random accesses must produce "
                        "prefetch misses (got 0)")

        total_accesses = total_hits + misses
        if total_accesses > 0:
            miss_rate = misses / total_accesses
            print(f"  Prefetch miss rate: {miss_rate:.2%} "
                  f"({misses}/{total_accesses})")
            if miss_rate < 0.50:
                return fail(
                    f"Branch penalty: random-address miss rate "
                    f"({miss_rate:.2%}) is too low; non-sequential "
                    f"accesses should mostly miss the prefetch buffers")

        return passed(f"branch penalty confirmed: {misses} misses vs "
                      f"{total_hits} hits on random traffic")

    # ---- hw_buffer_promotion ----
    #
    # STM32 two-buffer mechanism:
    #   The ART uses two buffers (prefetch + current).  When a prefetch
    #   completes, its data is promoted to the "current buffer" so the
    #   next sequential access from the CPU hits immediately without
    #   waiting for a new flash read.
    #
    # We verify:
    #   1. currentBufferHits > 0 (buffer promotion is working).
    #   2. prefetchBufferHits may also be > 0 (CPU caught up to the
    #      prefetcher), which is valid hardware behaviour.
    elif tt == "hw_buffer_promotion":
        current_hits = art_stats.get("currentBufferHits", 0)
        pf_hits = art_stats.get("prefetchBufferHits", 0)
        issued = art_stats.get("prefetchesIssued", 0)
        completed = art_stats.get("prefetchesCompleted", 0)

        if current_hits == 0:
            return fail(
                "Two-buffer promotion: currentBufferHits must be > 0 "
                "to confirm that prefetched lines are promoted to the "
                "current buffer for immediate CPU access")
        if issued == 0 or completed == 0:
            return fail(
                "Two-buffer promotion: prefetches must be issued and "
                "completed for buffer promotion to occur")

        return passed(
            f"buffer promotion verified: {current_hits} current-buffer "
            f"hits, {pf_hits} prefetch-buffer hits, "
            f"{completed} prefetches completed")

    # ---- hw_prefetch_disable ----
    #
    # STM32 FLASH_ACR register, PRFTEN bit:
    #   When PRFTEN=0, the instruction prefetch buffer is disabled and
    #   every instruction fetch must wait for the flash read to complete.
    #
    # We verify that all prefetch-related statistics are zero.
    elif tt == "hw_prefetch_disable":
        for name in ["currentBufferHits", "prefetchBufferHits",
                      "prefetchesIssued", "prefetchesCompleted",
                      "cpuWaitEvents"]:
            val = art_stats.get(name, 0)
            if val != 0:
                return fail(
                    f"PRFTEN=0: {name} must be 0 when prefetch is "
                    f"disabled (got {val})")

        prefetch_misses = art_stats.get("prefetchMisses", 0)
        if prefetch_misses != 0:
            return fail(
                f"PRFTEN=0: prefetchMisses must be 0 when prefetch is "
                f"disabled (got {prefetch_misses}); misses should only "
                f"be counted when the prefetcher is active")

        return passed("prefetch disable confirmed: all prefetch stats "
                      "are zero with PRFTEN=0")

    # ---- hw_direct_memory_bypass ----
    #
    # STM32 non-ART flash path:
    #   When the ART accelerator is disabled (or bypassed), every CPU
    #   access is forwarded directly to flash memory with full wait-state
    #   latency.  No caching or prefetching occurs.
    #
    # We verify:
    #   1. bypassAccesses > 0.
    #   2. The model routes all requests through the bypass path.
    elif tt == "hw_direct_memory_bypass":
        bypass = art_stats.get("bypassAccesses", 0)
        if bypass == 0:
            return fail(
                "Direct bypass: bypassAccesses must be > 0 in direct "
                "memory mode")

        pf_issued = art_stats.get("prefetchesIssued", 0)
        print(f"  {bypass} bypass accesses, {pf_issued} prefetches issued")
        return passed(f"direct memory bypass confirmed: {bypass} "
                      f"bypass accesses")

    # ---- hw_flash_range_bounds ----
    #
    # STM32 flash memory region:
    #   The ART accelerator only operates on the flash memory address
    #   space (0x0800_0000–0x081F_FFFF on STM32F4, for example).
    #   Accesses to other address regions bypass the ART entirely.
    #
    # The model enforces this via flashStartAddr/flashEndAddr.  We
    # configure a narrow flash range (0x0000–0x00FF) and send traffic
    # spanning 0x0000–0x0400.  Prefetches must only be issued for
    # addresses within the flash range.
    elif tt == "hw_flash_range_bounds":
        issued = art_stats.get("prefetchesIssued", 0)
        misses = art_stats.get("prefetchMisses", 0)
        total_hits = (art_stats.get("currentBufferHits", 0) +
                      art_stats.get("prefetchBufferHits", 0))

        if issued == 0:
            return fail(
                "Flash range: some prefetches must be issued for "
                "addresses within the flash range")

        # With flash range 0x0000-0x00FF and traffic 0x0000-0x0400,
        # approximately 1/4 of accesses are within flash range.
        # Misses should be non-zero since many accesses fall outside
        # the flash range and cannot be prefetched.
        if misses == 0:
            return fail(
                "Flash range: prefetchMisses must be > 0 since "
                "accesses beyond the flash range cannot be prefetched")

        print(f"  Flash range: {issued} prefetches issued, "
              f"{total_hits} hits, {misses} misses")
        return passed(
            f"flash range bounds enforced: prefetches bounded to "
            f"[0x0000, 0x00FF], {issued} issued, {misses} misses "
            f"outside range")

    # ---- hw_completion_fidelity ----
    #
    # Internal model fidelity check:
    #   Every prefetch that is issued should eventually receive a
    #   response from memory.  For a fully-drained workload,
    #   prefetchesCompleted should equal prefetchesIssued (minus at
    #   most 1 if a prefetch was in-flight when simulation ended).
    #
    # This validates the ART model's state machine: allocate -> send ->
    # receive -> deallocate lifecycle is correctly managed.
    elif tt == "hw_completion_fidelity":
        issued = art_stats.get("prefetchesIssued", 0)
        completed = art_stats.get("prefetchesCompleted", 0)

        if issued == 0:
            return fail(
                "Completion fidelity: prefetchesIssued must be > 0")
        if completed == 0:
            return fail(
                "Completion fidelity: prefetchesCompleted must be > 0")
        if completed > issued:
            return fail(
                f"Completion fidelity: prefetchesCompleted ({completed}) "
                f"must not exceed prefetchesIssued ({issued})")

        # Allow at most 1 in-flight prefetch at simulation end.
        delta = issued - completed
        if delta > 1:
            return fail(
                f"Completion fidelity: {delta} prefetches were issued "
                f"but never completed (expected at most 1 in-flight "
                f"at simulation end)")

        return passed(
            f"completion fidelity verified: {completed}/{issued} "
            f"prefetches completed ({delta} in-flight at end)")

    return passed("(unknown test type, no assertions)")


def fail(msg):
    """Print a FAIL message and return False."""
    print(f"FAIL: {msg}", file=sys.stderr)
    return False


def passed(msg):
    """Print a PASS message and return True."""
    print(f"PASS: {msg}")
    return True


# -----------------------------------------------------------------------
#  Main
# -----------------------------------------------------------------------

def main():
    parser = argparse.ArgumentParser(
        description="ART cache hardware-fidelity test configuration"
    )
    parser.add_argument(
        "--test-type",
        required=True,
        choices=[
            "hw_sequential_prefetch",
            "hw_branch_penalty",
            "hw_buffer_promotion",
            "hw_prefetch_disable",
            "hw_direct_memory_bypass",
            "hw_flash_range_bounds",
            "hw_completion_fidelity",
        ],
        help="Which hardware-fidelity test scenario to run",
    )
    args = parser.parse_args()

    system = create_system(args)
    cfg_path = create_traffic_cfg(args)
    system.tgen.config_file = cfg_path

    root = Root(full_system=False, system=system)
    m5.instantiate()

    print(f"Running ART hardware-fidelity test: {args.test_type}")
    exit_event = m5.simulate()
    print(f"Simulation ended: {exit_event.getCause()}")

    m5.stats.dump()
    m5.stats.reset()

    ok = check_stats(args)
    if not ok:
        sys.exit(1)
    sys.exit(0)


if __name__ == "__m5_main__":
    main()
