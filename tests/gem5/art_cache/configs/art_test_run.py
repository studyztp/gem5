"""
Hardware-fidelity test configuration for the ART (Adaptive Real-Time)
accelerator cache model.

Each test scenario maps to a specific behaviour documented in the STM32
Adaptive Real-Time memory accelerator specification (PM0059 Section 2.4.2,
RM0090 Section 3.4, and related STM32 reference manuals).

The system is intentionally simplified — zero-latency crossbar and a
4-cycle memory latency matching STM32 flash wait states — so that the
ART prefetcher's logic is fully isolated and every stat is deterministic
and computable from first principles by the companion reference model
(art_reference_model.py).
"""

import argparse
import os
import sys
import tempfile

from art_reference_model import (
    BLOCK_SIZE,
    BRANCH_SEQ_LEN,
    BRANCH_STATE_DURATION,
    BRANCH_TARGETS,
    BYPASS_PERIOD,
    DURATION,
    PF_BLK_SIZE,
    SEQ_PERIOD,
    SHORT_DURATION,
    compute_expected_stats,
)

import m5
from m5.objects import *
from m5.params import MaxAddr

# -----------------------------------------------------------------------
#  System construction
# -----------------------------------------------------------------------


def create_system(args):
    """Build a minimal system with TrafficGen -> ARTCache -> SimpleMemory.

    External latencies are zeroed out so that the only timing that
    matters is the ART's own logic plus the known 4-cycle memory
    latency (matching STM32 flash wait states at 1 GHz).
    """
    system = System()
    system.clk_domain = SrcClockDomain(
        clock="1GHz", voltage_domain=VoltageDomain()
    )
    system.mem_mode = "timing"
    system.mem_ranges = [AddrRange("1MiB")]

    # gem5 requires cache_line_size >= 32.  SectorTags lets us keep
    # 8-byte cache blocks while the indexing entry size is
    # block_size * num_blocks_per_sector = 8 * 4 = 32, matching
    # system.cache_line_size.
    system.cache_line_size = 32

    system.membus = IOXBar(
        width=64,
        frontend_latency=0,
        forward_latency=0,
        response_latency=0,
    )

    system.mem_ctrl = SimpleMemory(
        range=AddrRange("1MiB"),
        latency="4ns",
        bandwidth="0GiB/s",
    )
    system.mem_ctrl.port = system.membus.mem_side_ports
    system.system_port = system.membus.cpu_side_ports

    art_params = dict(
        size="1KiB",
        assoc=32,
        tag_latency=1,
        data_latency=1,
        response_latency=0,
        mshrs=1,
        tgts_per_mshr=4,
        is_read_only=True,
        pf_blk_size=PF_BLK_SIZE,
        cache_blk_size=8,
        tags=SectorTags(
            num_blocks_per_sector=4,
            block_size=8,
        ),
    )

    tt = args.test_type

    if tt in (
        "hw_sequential_prefetch",
        "hw_branch_penalty",
        "hw_buffer_promotion",
        "hw_completion_fidelity",
    ):
        art_params["enable_prefetch"] = True
        art_params["direct_memory_mode"] = False
        art_params["flash_start_addr"] = 0
        art_params["flash_end_addr"] = MaxAddr

    elif tt == "hw_prefetch_disable":
        art_params["enable_prefetch"] = False
        art_params["direct_memory_mode"] = False

    elif tt == "hw_direct_memory_bypass":
        art_params["enable_prefetch"] = False
        art_params["direct_memory_mode"] = True

    elif tt == "hw_flash_range_bounds":
        art_params["enable_prefetch"] = True
        art_params["direct_memory_mode"] = False
        art_params["flash_start_addr"] = 0x0000
        art_params["flash_end_addr"] = 0x00FF

    else:
        print(f"Unknown test type: {tt}", file=sys.stderr)
        sys.exit(1)

    art_params["prefetch_on_cache_hit"] = args.prefetch_on_cache_hit
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

    The request size is 8 bytes (instruction fetch block), modelling
    the STM32 ART accelerator's instruction line fetch granularity.

    With 4-cycle memory latency and 20-cycle request period, every
    prefetch (including follow-up chains) completes before the next
    CPU request arrives.
    """
    cfg_path = os.path.join(tempfile.gettempdir(), "art_tgen.cfg")
    tt = args.test_type

    if tt == "hw_sequential_prefetch":
        lines = [
            f"STATE 0 {DURATION} LINEAR 100 0 4096 {BLOCK_SIZE} "
            f"{SEQ_PERIOD} {SEQ_PERIOD} 0",
            f"STATE 1 {DURATION} EXIT",
            "INIT 0",
            "TRANSITION 0 1 1",
            "TRANSITION 1 1 1",
        ]

    elif tt == "hw_branch_penalty":
        lines = _build_branch_penalty_cfg()

    elif tt == "hw_buffer_promotion":
        lines = [
            f"STATE 0 {DURATION} LINEAR 100 0 2048 {BLOCK_SIZE} "
            f"{SEQ_PERIOD} {SEQ_PERIOD} 0",
            f"STATE 1 {DURATION} EXIT",
            "INIT 0",
            "TRANSITION 0 1 1",
            "TRANSITION 1 1 1",
        ]

    elif tt == "hw_prefetch_disable":
        lines = [
            f"STATE 0 {DURATION} LINEAR 100 0 4096 {BLOCK_SIZE} "
            f"{SEQ_PERIOD} {SEQ_PERIOD} 0",
            f"STATE 1 {DURATION} EXIT",
            "INIT 0",
            "TRANSITION 0 1 1",
            "TRANSITION 1 1 1",
        ]

    elif tt == "hw_direct_memory_bypass":
        lines = [
            f"STATE 0 {DURATION} LINEAR 100 0 4096 {BLOCK_SIZE} "
            f"{BYPASS_PERIOD} {BYPASS_PERIOD} 0",
            f"STATE 1 {DURATION} EXIT",
            "INIT 0",
            "TRANSITION 0 1 1",
            "TRANSITION 1 1 1",
        ]

    elif tt == "hw_flash_range_bounds":
        lines = [
            f"STATE 0 {DURATION} LINEAR 100 0 1024 {BLOCK_SIZE} "
            f"{SEQ_PERIOD} {SEQ_PERIOD} 0",
            f"STATE 1 {DURATION} EXIT",
            "INIT 0",
            "TRANSITION 0 1 1",
            "TRANSITION 1 1 1",
        ]

    elif tt == "hw_completion_fidelity":
        lines = [
            f"STATE 0 {SHORT_DURATION} LINEAR 100 0 512 {BLOCK_SIZE} "
            f"{SEQ_PERIOD} {SEQ_PERIOD} 0",
            f"STATE 1 {DURATION} EXIT",
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


def _build_branch_penalty_cfg():
    """
    Build a deterministic non-sequential traffic pattern for the
    branch-penalty test.

    Each of the BRANCH_TARGETS addresses becomes a separate LINEAR
    state that emits exactly one request (data_limit = BLOCK_SIZE).
    The states cycle: 0 -> 1 -> ... -> N-1 -> 0 -> 1 -> ...

    Each state lasts BRANCH_STATE_DURATION (= 2 * SEQ_PERIOD) so
    that the first packet fires before the state transitions.  The
    packet period within each state is SEQ_PERIOD, and
    data_limit=BLOCK_SIZE ensures only one packet per state visit.

    There is no EXIT state; the simulation is time-limited via
    m5.simulate(DURATION) instead.
    """
    n = len(BRANCH_TARGETS)
    lines = []

    for i, target in enumerate(BRANCH_TARGETS):
        end_addr = target + BLOCK_SIZE
        lines.append(
            f"STATE {i} {BRANCH_STATE_DURATION} LINEAR 100 "
            f"{target} {end_addr} {BLOCK_SIZE} "
            f"{SEQ_PERIOD} {SEQ_PERIOD} "
            f"{BLOCK_SIZE}"
        )

    lines.append(f"INIT 0")

    for i in range(n):
        next_state = (i + 1) % n
        lines.append(f"TRANSITION {i} {next_state} 1")

    return lines


# -----------------------------------------------------------------------
#  Statistics verification
# -----------------------------------------------------------------------


def parse_art_stats(stat_names):
    """Read ART-specific statistics from the stats.txt output file."""
    art_stats = {}
    stats_file = m5.options.outdir + "/stats.txt"

    with open(stats_file) as f:
        for line in f:
            for name in stat_names:
                if f"system.art_cache.{name}" in line:
                    parts = line.split()
                    art_stats[name] = int(float(parts[1]))
    return art_stats


def check_stats(args):
    """
    Validate ART statistics against the reference model.

    The reference model (art_reference_model.py) computes exact
    expected values by replaying the same address sequence through
    a pure-Python implementation of the ART state machine.  Every
    stat must match exactly — gem5 simulation with TrafficGen is
    fully deterministic.
    """
    stat_names = [
        "currentBufferHits",
        "prefetchBufferHits",
        "prefetchMisses",
        "bypassAccesses",
        "cpuWaitEvents",
        "prefetchesAllocated",
        "prefetchesIssued",
        "prefetchesCompleted",
    ]
    art_stats = parse_art_stats(stat_names)
    expected = compute_expected_stats(
        args.test_type,
        prefetch_on_cache_hit=args.prefetch_on_cache_hit,
    )

    print(f"\nART statistics for {args.test_type}:")
    print(f"  {'stat':<30s} {'actual':>8s} {'expected':>8s}  {'match':>5s}")
    print(f"  {'-'*30} {'-'*8} {'-'*8}  {'-'*5}")

    all_match = True
    for name in stat_names:
        actual = art_stats.get(name, -1)
        exp = expected.get(name, -1)
        match = actual == exp
        marker = "OK" if match else "FAIL"
        print(f"  {name:<30s} {actual:>8d} {exp:>8d}  {marker:>5s}")
        if not match:
            all_match = False

    if all_match:
        return passed(f"all {len(stat_names)} stats match the reference model")
    else:
        return fail(
            "one or more stats differ from the reference model — "
            "see table above"
        )


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
    parser.add_argument(
        "--trace",
        action="store_true",
        help="Enable ARTCache debug trace output (DPRINTF)",
    )
    parser.add_argument(
        "--prefetch-on-cache-hit",
        action="store_true",
        help="Issue prefetch immediately when the I-Cache hits "
        "(default: prefetch stays ALLOC and is discarded)",
    )
    args = parser.parse_args()

    system = create_system(args)
    cfg_path = create_traffic_cfg(args)
    system.tgen.config_file = cfg_path

    root = Root(full_system=False, system=system)
    m5.instantiate()

    if args.trace:
        m5.debug.flags["ARTCache"].enable()
        m5.debug.flags["TrafficGen"].enable()

    print(f"Running ART hardware-fidelity test: {args.test_type}")

    if args.test_type == "hw_branch_penalty":
        exit_event = m5.simulate(DURATION)
    else:
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
