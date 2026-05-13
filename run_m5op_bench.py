#!/usr/bin/env python3
"""
Run a microbenchmark with m5ops ROI (work_begin/work_end) on STM32G474RE
using the signal-driven 3-stage Cortex-M4 CPU (SignalCPU) and the latest
calibrated memory timings.

CPU: ArmMSignalCPU (signal-driven 3-stage in-order; commit e09b461cb &
     671629f89 — E->D flag fwd, D-side T1 Bcond resolve).
Boards (defaults recalibrated 2026-05-13 from nop16 sweep slope match):
  --no-art : STM32G474RETunableBoard
             flash 29411ps / addr_phase 600ps / buf_hit 5882ps / rbuf 8
             (sum 35293ps = 6 HCLK/line @ 170MHz → HW slope 1.5 cy/NOP)
  default  : STM32G474RETunableBoardART
             flash 29000ps / addr_phase 600ps / buf_hit 5000ps,
             pipelined ART buffer hits, AHB-port buffer size 8.
             flash latency now matches the no-ART board's value
             (the silicon flash is the same regardless of whether
             ART is enabled).
             ART cache + prefetch can be toggled independently via
             --no-art-cache / --no-art-prefetch (each defaults on).

The benchmark firmware uses gem5 m5ops via semihosting:
  - m5_work_begin()  -> enables debug flags, resets stats
  - m5_work_end()    -> disables debug flags, dumps stats

Usage:
    ./gem5/build/ARM/gem5.opt -re --outdir=m5out_bench \\
        gem5/run_m5op_bench.py \\
        --firmware path/to/bench.elf \\
        [--no-art] \\
        [--debug-flags Signal3CPUExecute,Signal3CPUFetch] \\
        [--tick-limit 10000000000000]
"""

import argparse
import os
import re
import shutil
import subprocess

import m5
from m5 import debug as m5_debug
from m5.objects import Root
from m5.objects.ArmMCPU import ArmMSignalCPU
from m5.objects.ArmSemihosting import ArmSemihosting
from m5.stats import dump as stats_dump
from m5.stats import reset as stats_reset

from gem5.prebuilt.cortexm.boards import (
    STM32G474RETunableBoard,
    STM32G474RETunableBoardART,
)

CLK_FREQ_HZ = 170_000_000
TICK_PER_SEC = 1_000_000_000_000  # gem5 ticks = picoseconds

parser = argparse.ArgumentParser(
    description="Run STM32G474RE benchmark with m5ops ROI on SignalCPU"
)
parser.add_argument("--firmware", required=True, help="Path to benchmark .elf")
parser.add_argument(
    "--tick-limit",
    type=int,
    default=10_000_000_000_000,
    help="Max ticks before abort (default 10ms)",
)
parser.add_argument(
    "--no-art",
    action="store_true",
    help="Disable ART caches (use STM32G474RETunableBoard with raw "
    "flash latency on every access). Default: ART enabled via "
    "STM32G474RETunableBoardART.",
)
parser.add_argument(
    "--debug-flags",
    type=str,
    default="",
    help="Comma-separated debug flags to enable during ROI "
    "(e.g., Signal3CPUExecute,Signal3CPUFetch)",
)
parser.add_argument(
    "--inner-reps",
    type=int,
    default=1,
    help="Number of inner repetitions in the kernel loop "
    "(for computing per-call cycles)",
)
parser.add_argument(
    "--debug-from-start",
    action="store_true",
    help="If set, start printing debug info from start",
)
parser.add_argument(
    "--progress-interval",
    type=int,
    default=0,
    help="Print progress every N ticks (0 = disabled). "
    "E.g., 1000000000 for every 1M ticks (~170 cycles).",
)
parser.add_argument(
    "--roi-counter",
    type=int,
    default=1,
    help="Initial value of the internal roi_counter. The script "
    "measures the (N+1)-th m5_work_begin / work_end pair; the "
    "first N events are skipped. Default 1 = treat Rep 0 as warmup "
    "and measure Rep 1 (matches the cycle-benchmark REPS=2 "
    "convention used by stm32-board-microbenchmarks). Pass 0 for "
    "single-ROI binaries (DO_WARMUP=0 + REPS=1 diff-style tests).",
)
parser.add_argument(
    "--run-to-exit",
    action="store_true",
    help="Keep simulating after the first work_end until the "
    "firmware exits naturally. Required when the firmware prints "
    "data (e.g. EntoBench ENTO_RESULT lines) after the ROI.",
)

# --- No-ART (TunableBoard) flash timing knobs ---
no_art = parser.add_argument_group(
    "no-ART flash timing (only used with --no-art)",
    "STM32G474RETunableBoard parameters; defaults are the 2026-05-13 "
    "nop16-slope-matched values (latency+buf_hit = 6 HCLK/line).",
)
no_art.add_argument(
    "--flash-latency",
    default="29411ps",
    help="PipelinedSimpleMemory.latency for flash (default 29411ps "
    "= 5 HCLK = 4 wait states + 1 access cycle, RM0440 §3.3.3).",
)
no_art.add_argument(
    "--flash-address-phase-latency",
    default="600ps",
    help="PipelinedSimpleMemory.address_phase_latency (default 600ps).",
)
no_art.add_argument(
    "--flash-buffer-hit-latency",
    default="5882ps",
    help="PipelinedSimpleMemory.buffer_hit_latency (default 5882ps "
    "= 1 HCLK at 170 MHz = AHB address phase after sense-amp hit).",
)
no_art.add_argument(
    "--flash-read-buffer-size",
    type=int,
    default=8,
    help="PipelinedSimpleMemory.port_read_buffer_size per port "
    "(default 8 = 64-bit Flash sense-amp latch).",
)

# --- ART (TunableBoardART) timing knobs ---
art = parser.add_argument_group(
    "ART timing (default, omit --no-art)",
    "STM32G474RETunableBoardART parameters.",
)
art.add_argument(
    "--art-flash-latency",
    default="29000ps",
    help="SimpleMemory.latency for flash behind ART (default 29000ps; "
    "matches no-ART board's flash_latency since the underlying "
    "silicon flash takes the same time regardless of whether ART is "
    "enabled — RM0440 §4 Table 19, 4 wait states + 1 access cycle = "
    "5 HCLK at 170 MHz).",
)
art.add_argument(
    "--art-address-phase-latency",
    default="600ps",
    help="ARTCache.address_phase_latency (default 600ps).",
)
art.add_argument(
    "--art-prefetch-buffer-hit-latency",
    "--art-buffer-hit-latency",  # legacy alias
    dest="art_prefetch_buffer_hit_latency",
    default="5000ps",
    help="ARTCache.prefetch_buffer_hit_latency — latency for serving "
    "from the ART current/prefetch buffer (Cases B1/B2).  Distinct "
    "from --art-port-ahb-buffer-latency (Case A).  Default 5000ps.",
)
art.add_argument(
    "--no-art-pipeline",
    dest="art_enable_pipeline",
    action="store_false",
    help="Disable pipelined ART buffer hits (default: enabled).",
)
parser.set_defaults(art_enable_pipeline=True)
art.add_argument(
    "--art-arrive-buffer-size",
    type=int,
    default=1,
    help="ARTCache.arrive_buffer_size (default 1).",
)
art.add_argument(
    "--art-port-ahb-buffer-size",
    type=int,
    default=8,
    help="ARTCache.port_ahb_buffer_size (default 8).",
)
art.add_argument(
    "--art-port-ahb-buffer-latency",
    default="0ns",
    help="ARTCache.port_ahb_buffer_latency (default 0ns).",
)
art.add_argument(
    "--no-art-cache",
    dest="art_enable_cache",
    action="store_false",
    help="Disable the ART data cache "
    "(ARTCache.direct_memory_mode=True).  Demand misses bypass the "
    "underlying cache and go straight to flash; cache fills are "
    "skipped.  Per-bank prefetch buffers still serve hits unless "
    "--no-art-prefetch is also passed.  Default: cache enabled.",
)
parser.set_defaults(art_enable_cache=True)
art.add_argument(
    "--no-art-prefetch",
    dest="art_enable_prefetch",
    action="store_false",
    help="Disable ART sequential instruction prefetching "
    "(ARTCache.enable_prefetch=False).  The cache still fills/hits "
    "unless --no-art-cache is also passed.  Default: prefetch "
    "enabled.",
)
parser.set_defaults(art_enable_prefetch=True)
art.add_argument(
    "--art-max-outstanding-requests",
    type=int,
    default=2,
    help="ARTCache.max_outstanding_requests (AHB-Lite back-to-back "
    "depth: 1 = strict serialization, 2 = address-phase / data-phase "
    "pipelining, default 2).",
)
art.add_argument(
    "--no-art-psm-compatible-bypass",
    dest="art_psm_compatible_bypass",
    action="store_false",
    help="When ART cache+prefetch are both off, skip the PSM-style "
    "fast path and use the legacy bypassCacheEntry path.  For A/B "
    "testing during the refactor.  Default: PSM-compatible (faster).",
)
parser.set_defaults(art_psm_compatible_bypass=True)

# --- SignalCPU param overrides (apply post-board, pre-instantiate) ---
# Exposed so the cycle-parity experiments can sweep front-end knobs
# without rebuilding gem5. Each defaults to None → leave the SimObject
# default (see gem5/src/cpu/signal-3-stage-in-order-cpu/SignalCPU.py).
cpu_knobs = parser.add_argument_group(
    "SignalCPU overrides",
    "Per-run overrides applied to board.cpu before m5.instantiate(). "
    "Leave unset to use SignalCPU.py defaults.",
)
cpu_knobs.add_argument(
    "--pfu-fifo-words",
    type=int,
    default=None,
    help="Override SignalCPU.pfu_fifo_words (prefetch FIFO depth in "
    "32-bit words; SimObject default 3).",
)
cpu_knobs.add_argument(
    "--pfu-max-outstanding-fetches",
    type=int,
    default=None,
    help="Override SignalCPU.pfu_max_outstanding_fetches (AHB-Lite "
    "in-flight cap; SimObject default 2 = address+data phase pipelining).",
)

args = parser.parse_args()

debug_from_start = args.debug_from_start

# --- Board setup ---
if args.no_art:
    board = STM32G474RETunableBoard(
        cpu_cls=ArmMSignalCPU,
        flash_latency=args.flash_latency,
        flash_address_phase_latency=args.flash_address_phase_latency,
        flash_buffer_hit_latency=args.flash_buffer_hit_latency,
        flash_read_buffer_size=args.flash_read_buffer_size,
    )
    print(
        f"Board: STM32G474RETunableBoard (no-ART) "
        f"+ ArmMSignalCPU\n  {board.tunable_flash_summary()}"
    )
else:
    board = STM32G474RETunableBoardART(
        cpu_cls=ArmMSignalCPU,
        art_flash_latency=args.art_flash_latency,
        art_address_phase_latency=args.art_address_phase_latency,
        art_prefetch_buffer_hit_latency=args.art_prefetch_buffer_hit_latency,
        art_enable_pipeline=args.art_enable_pipeline,
        art_arrive_buffer_size=args.art_arrive_buffer_size,
        art_port_ahb_buffer_size=args.art_port_ahb_buffer_size,
        art_port_ahb_buffer_latency=args.art_port_ahb_buffer_latency,
        art_enable_cache=args.art_enable_cache,
        art_enable_prefetch=args.art_enable_prefetch,
        art_max_outstanding_requests=args.art_max_outstanding_requests,
        art_psm_compatible_bypass=args.art_psm_compatible_bypass,
    )
    print(
        f"Board: STM32G474RETunableBoardART (ART on) "
        f"+ ArmMSignalCPU\n  {board.tunable_art_summary()}"
    )

board.semihosting = ArmSemihosting(
    mem_reserve="0B",  # Don't reserve memory — firmware manages its own heap
    stack_size="0B",  # Don't reserve stack — firmware sets SP from vector table
)
board.set_workload(args.firmware)

# Apply SignalCPU param overrides (post-board, pre-instantiate). The
# board has already constructed cpu_cls(...) with its own defaults; we
# poke the python attrs here so m5.instantiate() picks them up.
if args.pfu_fifo_words is not None:
    board.cpu.pfu_fifo_words = args.pfu_fifo_words
    print(f"  SignalCPU override: pfu_fifo_words = {args.pfu_fifo_words}")
if args.pfu_max_outstanding_fetches is not None:
    board.cpu.pfu_max_outstanding_fetches = args.pfu_max_outstanding_fetches
    print(
        f"  SignalCPU override: pfu_max_outstanding_fetches = "
        f"{args.pfu_max_outstanding_fetches}"
    )

root = Root(full_system=True, system=board)

# Enable exit events on m5_work_begin / m5_work_end
board.exit_on_work_items = True

# Parse debug flags but don't enable yet — wait for ROI
roi_flags = [f.strip() for f in args.debug_flags.split(",") if f.strip()]

# Don't enable any debug flags at startup — wait for ROI.
# The C++ runtime startup can take millions of instructions;
# tracing all of them would be extremely slow.

m5.instantiate()

# --- Simulation loop: handle m5ops events ---
roi_start_tick = None
roi_end_tick = None

print(f"Starting simulation (ROI flags: {roi_flags or 'none'})...")

if debug_from_start:
    for flag_name in roi_flags:
        if flag_name in m5_debug.flags:
            m5_debug.flags[flag_name].enable()

roi_counter = args.roi_counter
progress_interval = args.progress_interval

while True:
    if progress_interval > 0:
        remaining = args.tick_limit - m5.curTick()
        sim_ticks = min(progress_interval, remaining)
    else:
        sim_ticks = args.tick_limit - m5.curTick()

    exit_event = m5.simulate(sim_ticks)
    cause = exit_event.getCause()

    if cause == "simulate() limit reached":
        if progress_interval > 0 and m5.curTick() < args.tick_limit:
            ticks_per_cycle = TICK_PER_SEC // CLK_FREQ_HZ
            cycles = m5.curTick() / ticks_per_cycle
            print(
                f"  [progress] tick={m5.curTick():,}  "
                f"cycle={cycles:.0f}  "
                f"({m5.curTick() * 100 / args.tick_limit:.1f}%)",
                flush=True,
            )
            continue
        else:
            print(f"\nTick limit reached at {m5.curTick()}")
            break

    if cause == "workbegin":
        if roi_counter != 0:
            continue
        roi_start_tick = m5.curTick()
        print(f"\n*** ROI BEGIN at tick {roi_start_tick} ***")
        stats_reset()
        # Auto-enable Signal3CPULineTrace: emits one row per cycle
        # showing the end-of-cycle fetch / decode / execute state. The
        # precise kernel-only cycle extractor below walks those rows
        # and counts cycles whose E-stage slot PC lies inside the
        # `kernel_body` symbol's flash range. Opt-out via the
        # NoLineTrace sentinel in --debug-flags.
        if "NoLineTrace" not in roi_flags:
            m5_debug.flags["Signal3CPULineTrace"].enable()
            # PipelinedMemLineTrace is no longer auto-enabled — the
            # post-2026-05-13 precise extractor reads kernel-window
            # start/end purely from Signal3CPULineTrace's `F.req=`
            # (front-end issued a fetch for a kernel addr) and
            # `F.arr=[...]` (kernel word delivered into the FIFO).
            # If you want the flash-side trace for diagnostics, pass
            # `--debug-flags PipelinedMemLineTrace` explicitly.
        else:
            print(
                "  Skipping Signal3CPULineTrace auto-enable "
                "(NoLineTrace sentinel in flags)"
            )
        for flag_name in roi_flags:
            if flag_name in m5_debug.flags:
                m5_debug.flags[flag_name].enable()
                print(f"  Enabled debug flag: {flag_name}")
            else:
                print(f"  WARNING: Unknown debug flag '{flag_name}'")

    elif cause == "workend":
        if roi_counter != 0:
            roi_counter -= 1
        else:
            roi_end_tick = m5.curTick()
            print(f"\n*** ROI END at tick {roi_end_tick} ***")
            m5_debug.flags["Signal3CPULineTrace"].disable()
            for flag_name in roi_flags:
                if flag_name in m5_debug.flags:
                    m5_debug.flags[flag_name].disable()
            stats_dump()

            if roi_start_tick is not None:
                delta = roi_end_tick - roi_start_tick
                ticks_per_cycle = TICK_PER_SEC // CLK_FREQ_HZ
                cycles = delta / ticks_per_cycle
                print(f"  ROI ticks : {delta}")
                print(
                    f"  ROI cycles: {cycles:.1f}  (at {CLK_FREQ_HZ / 1e6:.0f} MHz)"
                )

            # Default behavior: stop here, the cycle delta is already
            # collected. With --run-to-exit, keep simulating so the
            # firmware can print post-ROI output (e.g. ENTO_RESULT)
            # and then exit naturally — the "Stopped"/"exit" branch
            # below will catch the firmware's final m5_exit / abort.
            if args.run_to_exit:
                print("\nROI complete; continuing to firmware exit...")
                continue
            print(f"\nExiting after first ROI.")
            break

    elif "Stopped" in cause or "exit" in cause.lower():
        print(f"\nExiting @ tick {m5.curTick()} because {cause}")
        break

    else:
        print(f"\nUnhandled exit event: {cause} @ tick {m5.curTick()}")
        break

# --- Precise cycle counting from Signal3CPULineTrace ---
#
# We measure pure-body cycles as
#
#     pure_body = last_kernel_arr_cy - first_kernel_req_cy + 1
#
# where:
#   - "kernel" PC range is [kernel_body_start, kernel_body_end − 2)
#     (the trailing 2-byte `bx lr` is excluded — see
#     _kernel_pc_range below).
#   - "first_kernel_req_cy" is the first cycle the front-end issued
#     an icache fetch whose addr lies in the kernel range
#     (`F.req=0x<kernel-pc>` in the Signal3 line trace).
#   - "last_kernel_arr_cy" is the last cycle a kernel-PC word was
#     delivered into F's FIFO (`F.arr=[...,0x<kernel-pc>,...]` in
#     the Signal3 line trace).
#
# Why this definition: it matches the silicon DWT_CYCCNT inner window
# documented in stm32-board-microbenchmarks/microbenchmark/README.md
# §"What the inner window actually captures — pipeline-entry to
# fetch-completion".  Empirically, HW brackets "first kernel inst
# entering the pipeline" to "last kernel inst finishing its fetch
# from flash" — the +5 cy delta on `hw-none` between nop16_8 (29)
# and nop16_9 (34) is exactly one 64-bit flash line fetch, which
# is only inside the window if last-fetch-from-flash anchors the
# end.  The prior `data-delivery → commit` extractor anchored at
# the wrong boundary on the start side and lost the F→D→E pipeline
# fill (~3 cy) from the measurement; the prior framing constant of
# 17 was calibrated to compensate for that bias.  Switching to
# `req → arr` removes the bias; framing constants in
# experiments/.../scripts/_configs.py need to be re-calibrated by
# the offset for HW comparison.  See
# issues/2026-05-13-nop16-none-front-end-rate/ and
# issues/2026-05-13-signal3-linetrace-redesign/.
#
# Format expected from Signal3CPULineTrace (named-sub-field schema,
# 2026-05-13 line-trace redesign — see
# issues/2026-05-13-signal3-linetrace-redesign/):
#   <tick>: global: |  <cyc>  | <fetch-cell> | <decode-cell> | <execute-cell> |
# The fetch cell starts with `next=<pc>` and exposes the per-cycle
# events `req=<pc>|.`, `arr=[<pc>,...]|.`, `hold=[...]|.`, `i=<N>`.
# Both extractor anchors (`req=0x<pc>` and `arr=[0x<pc>,...]`) live
# inside this cell.
#
# PipelinedMemLineTrace is no longer required by the extractor; pass
# it via `--debug-flags PipelinedMemLineTrace` if you want the flash
# trace alongside for diagnostics.

# `global:` is specific to the CPU's line trace, distinguishing it
# from `system.mem_N:` mem trace lines.  Group 3 captures the F cell
# (anchored on the leading `next=`), groups 4 and 5 are decode +
# execute cells respectively.  Anchoring on `next=` filters out the
# detail-trace event rows (Signal3CPULineTraceDetail), whose 2nd
# cell is an event-tag string rather than F state.
_RE_LINETRACE = re.compile(
    r"^\s*(\d+):\s+global:\s+\|\s*(\d+)\s*\|"  # 1: tick, 2: cycle
    r"\s*(next=[^|]*?)\s*\|"  # 3: F cell
    r"\s*([^|]*?)\s*\|"  # 4: D cell
    r"\s*([^|]*?)\s*\|"  # 5: E cell
)
# Anchors inside the F cell.  `req=` carries at most one addr per
# cycle (the addr issued to icache this cycle); `arr=` is a list of
# zero or more addrs delivered into the FIFO this cycle.  A literal
# dot `.` means "no event this cycle"; we only act on hex addrs.
_RE_F_REQ = re.compile(r"req=0x([0-9a-fA-F]+)")
_RE_F_ARR = re.compile(r"arr=\[([^\]]+)\]")
# Find the commit anchor inside the execute cell.  Used only as a
# fallback (cache-hit configs where F.req / F.arr never fire for a
# kernel addr).
_RE_SLOT_PC = re.compile(r"commit=0x([0-9a-fA-F]+)/")

_TICK_PER_CY = TICK_PER_SEC // CLK_FREQ_HZ

ARM_NM = shutil.which("arm-none-eabi-nm") or "arm-none-eabi-nm"

# Size of the trailing `bx lr` emitted by END_BENCH (always 2-byte
# Thumb). Excluded from the kernel PC range so the precise count
# reflects just the user's instruction body.
_END_BENCH_BX_LR_SIZE = 2


def _kernel_pc_range(elf_path, symbol="kernel_body"):
    """Return (start_pc, end_pc_exclusive_of_bx_lr) for the symbol
    from `nm -S`, or None if absent. The size returned by nm includes
    the END_BENCH `bx lr`; we strip it here."""
    try:
        out = subprocess.check_output([ARM_NM, "-S", elf_path], text=True)
    except (subprocess.CalledProcessError, FileNotFoundError):
        return None
    for line in out.splitlines():
        parts = line.split()
        # `nm -S` sized row: addr size type name (4 cols)
        if len(parts) == 4 and parts[3] == symbol:
            start = int(parts[0], 16)
            size = int(parts[1], 16)
            if size <= _END_BENCH_BX_LR_SIZE:
                return None
            return start, start + size - _END_BENCH_BX_LR_SIZE
    return None


kernel_range = _kernel_pc_range(args.firmware)
if kernel_range is None:
    print(
        f"WARN: kernel_body not found in {args.firmware} — "
        "precise kernel-only extraction disabled."
    )

precise_start_cyc = None  # first kernel F.req cycle (preferred)
precise_end_cyc = None  # last kernel F.arr cycle (preferred)
precise_start_source = None  # "F.req" or fallback "commit"
precise_end_source = None  # "F.arr" or fallback "commit"
first_commit_cyc = None  # fallback start when F.req absent
last_commit_cyc = None  # fallback end when F.arr absent
simout_path = os.path.join(m5.options.outdir, "simout.txt")

try:
    if kernel_range is not None:
        ker_start, ker_end = kernel_range
        # Single pass over the Signal3 line trace.  For each summary
        # row we look at three fields:
        #   F.req=0x<pc>     : icache fetch issued this cycle
        #   F.arr=[0x<pc>,…] : word(s) delivered into FIFO this cycle
        #   E.commit=0x<pc>  : inst committed this cycle (fallback only)
        # First kernel-PC F.req → window start; last kernel-PC F.arr →
        # window end.  When neither fires for the kernel range (full
        # cache-hit configs like `art`), we fall back to commit-window.
        with open(simout_path) as f:
            for line in f:
                m = _RE_LINETRACE.match(line)
                if m is None:
                    continue
                cyc = int(m.group(2))
                f_cell = m.group(3)
                e_cell = m.group(5)

                # F.req — at most one addr per cycle.
                req_m = _RE_F_REQ.search(f_cell)
                if req_m is not None:
                    req_addr = int(req_m.group(1), 16)
                    if (
                        ker_start <= req_addr < ker_end
                        and precise_start_cyc is None
                    ):
                        precise_start_cyc = cyc
                        precise_start_source = "F.req"

                # F.arr — zero or more comma-separated addrs.
                arr_m = _RE_F_ARR.search(f_cell)
                if arr_m is not None:
                    for tok in arr_m.group(1).split(","):
                        try:
                            a = int(tok.strip(), 16)
                        except ValueError:
                            continue
                        if ker_start <= a < ker_end:
                            precise_end_cyc = cyc
                            precise_end_source = "F.arr"
                            # No break — the loop bumps precise_end_cyc
                            # to the latest matching cycle.

                # Commit fallback bookkeeping.
                pc_m = _RE_SLOT_PC.search(e_cell)
                if pc_m is not None:
                    pc = int(pc_m.group(1), 16)
                    if ker_start <= pc < ker_end:
                        if first_commit_cyc is None:
                            first_commit_cyc = cyc
                        last_commit_cyc = cyc

        # Fallbacks for cache-hit configs where F.req / F.arr never
        # carry a kernel addr (the cache absorbs the fetches without
        # the CPU's port firing).  Match the prior extractor's
        # cache-hit behaviour: anchor on commits.
        if precise_start_cyc is None and first_commit_cyc is not None:
            precise_start_cyc = first_commit_cyc
            precise_start_source = "commit"
        if precise_end_cyc is None and last_commit_cyc is not None:
            precise_end_cyc = last_commit_cyc
            precise_end_source = "commit"
except OSError:
    pass

# --- Final summary ---
print("\n" + "=" * 60)
bench_name = os.path.splitext(os.path.basename(args.firmware))[0]
print(f"Benchmark : {bench_name}")

ticks_per_cycle = TICK_PER_SEC // CLK_FREQ_HZ

inner_reps = args.inner_reps

if roi_start_tick is not None and roi_end_tick is not None:
    delta = roi_end_tick - roi_start_tick
    cycles = delta / ticks_per_cycle
    print(f"ROI Start : {roi_start_tick}")
    print(f"ROI End   : {roi_end_tick}")
    print(f"Delta     : {delta} ticks")
    print(
        f"Cycles (total)    : {cycles:.1f}  (at {CLK_FREQ_HZ / 1e6:.0f} MHz)"
    )
    if inner_reps > 1:
        print(
            f"Cycles (per call) : {cycles / inner_reps:.1f}  "
            f"(total / {inner_reps} inner_reps)"
        )

    if precise_start_cyc is not None and precise_end_cyc is not None:
        precise_cycles = precise_end_cyc - precise_start_cyc + 1
        start_label = {
            "F.req": "first kernel F.req (icache fetch issued)",
            "commit": "first kernel commit (fallback: no F.req fired)",
        }.get(precise_start_source, precise_start_source or "?")
        end_label = {
            "F.arr": "last kernel F.arr (word delivered to FIFO)",
            "commit": "last kernel commit (fallback: no F.arr fired)",
        }.get(precise_end_source, precise_end_source or "?")
        print(f"")
        print(f"Precise (kernel only, excluding m5op overhead):")
        print(f"  Source            : Signal3CPULineTrace (req → arr)")
        print(
            f"  Start cycle       : {precise_start_cyc}  " f"({start_label})"
        )
        print(f"  Last kernel cycle : {precise_end_cyc}  " f"({end_label})")
        print(f"  Cycles (total)    : {float(precise_cycles):.1f}")
        if inner_reps > 1:
            print(
                f"  Cycles (per call) : {precise_cycles / inner_reps:.1f}  "
                f"(total / {inner_reps} inner_reps)"
            )
else:
    missing = []
    if roi_start_tick is None:
        missing.append("work_begin")
    if roi_end_tick is None:
        missing.append("work_end")
    print(f"ROI INCOMPLETE: missing {', '.join(missing)}")

print("=" * 60)
