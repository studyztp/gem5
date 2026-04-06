#!/usr/bin/env python3
"""
Run a microbenchmark with m5ops ROI (work_begin/work_end) on STM32G474RE.

The benchmark firmware uses gem5 m5ops via semihosting:
  - m5_work_begin()  → enables debug flags, resets stats
  - m5_work_end()    → disables debug flags, dumps stats

Usage:
    ./gem5/build/ARM/gem5.opt -re --outdir=m5out_bench \
        microbenchmarks/run_m5op_bench.py \
        --firmware path/to/bench.elf \
        [--no-art] \
        [--debug-flags MinorExecute,MinorMem] \
        [--tick-limit 10000000000000]
"""

import argparse
import os
import re
import sys

import m5
from m5 import debug as m5_debug
from m5.objects import Root
from m5.objects.ArmSemihosting import ArmSemihosting
from m5.stats import dump as stats_dump
from m5.stats import reset as stats_reset

from gem5.prebuilt.cortexm.boards import STM32G474RETimingBoard

CLK_FREQ_HZ = 170_000_000
TICK_PER_SEC = 1_000_000_000_000  # gem5 ticks = picoseconds

parser = argparse.ArgumentParser(
    description="Run STM32G474RE benchmark with m5ops ROI"
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
    help="Disable ART caches (raw flash latency on every access)",
)
parser.add_argument(
    "--debug-flags",
    type=str,
    default="",
    help="Comma-separated debug flags to enable during ROI "
    "(e.g., MinorExecute,MinorMem)",
)
parser.add_argument(
    "--debug-from-start",
    action="store_true",
    help="If start printing debug info from start",
)
args = parser.parse_args()

debug_from_start = args.debug_from_start

# --- Board setup ---
board = STM32G474RETimingBoard(enable_art=not args.no_art)
board.semihosting = ArmSemihosting()
board.set_workload(args.firmware)

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

while True:
    exit_event = m5.simulate(args.tick_limit - m5.curTick())
    cause = exit_event.getCause()

    if cause == "workbegin":
        roi_start_tick = m5.curTick()
        print(f"\n*** ROI BEGIN at tick {roi_start_tick} ***")
        stats_reset()
        # Enable Exec trace during ROI for cycle-level analysis
        m5_debug.flags["Exec"].enable()
        for flag_name in roi_flags:
            if flag_name in m5_debug.flags:
                m5_debug.flags[flag_name].enable()
                print(f"  Enabled debug flag: {flag_name}")
            else:
                print(f"  WARNING: Unknown debug flag '{flag_name}'")

    elif cause == "workend":
        roi_end_tick = m5.curTick()
        print(f"\n*** ROI END at tick {roi_end_tick} ***")
        m5_debug.flags["Exec"].disable()
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

        print(f"\nExiting after first ROI.")
        break

    elif "Stopped" in cause or "exit" in cause.lower():
        print(f"\nExiting @ tick {m5.curTick()} because {cause}")
        break

    else:
        print(f"\nUnhandled exit event: {cause} @ tick {m5.curTick()}")
        break

# --- Precise cycle counting from Exec trace ---
# Find kernel start: first instruction after the work_begin bkpt returns.
# Find kernel end: instruction immediately before "movs r0, #91" (0x5B =
# M5OP_WORK_END argument) which starts the work_end call chain.
#
# Pattern in trace:
#   bkpt #0xab ; semihosting    ← work_begin returns
#   bx lr                       ← return from m5_semi_call
#   <kernel instructions>       ← MEASURE THIS
#   movs r0, #91                ← work_end arg setup (EXCLUDE)
#   bl.w <m5_semi_call>         ← work_end call

precise_start_tick = None
precise_end_tick = None
simout_path = os.path.join(m5.options.outdir, "simout.txt")

# Match: tick, symbol (with optional micro-op suffix), and instruction text
# Symbol examples: @main+289, @main+287. 4, @_ZL12m5_semi_callhmm+1
_RE_EXEC = re.compile(
    r"^\s*(\d+):\s+\S+:\s+(?:A\d+\s+)?T\d+\s*:\s*0x[0-9a-fA-F]+\s+"
    r"@(\S+?)(?:\.\s*\d+)?\s+:\s+(.*)"
)

try:
    in_roi = False
    prev_tick = None
    prev_main_tick = None

    with open(simout_path) as f:
        for line in f:
            # Extract tick from any trace line
            m_tick = re.match(r"^\s*(\d+):", line)
            if not m_tick:
                continue
            tick = int(m_tick.group(1))

            if roi_start_tick and tick >= roi_start_tick:
                in_roi = True
            if in_roi and roi_end_tick and tick > roi_end_tick:
                break
            if not in_roi:
                continue

            # Try to match full exec trace line with symbol
            m_obj = _RE_EXEC.match(line)
            if m_obj is None:
                continue

            symbol = m_obj.group(2)
            instr = m_obj.group(3).strip()

            # Kernel start: first @main instruction in ROI
            if (
                precise_start_tick is None
                and "main" in symbol
                and "m5_semi_call" not in symbol
                and "_ZN9EntoBench" not in symbol
                and "_ZL12m5_semi" not in symbol
            ):
                precise_start_tick = tick

            # Kernel end: "movs r0, #91" = M5OP_WORK_END arg setup.
            # The last @main instruction before this is the kernel end.
            if (
                precise_start_tick is not None
                and "movs" in instr
                and "#91" in instr
            ):
                precise_end_tick = prev_main_tick
                break

            # Track last @main instruction tick
            if (
                "main" in symbol
                and "m5_semi_call" not in symbol
                and "_ZN9EntoBench" not in symbol
            ):
                prev_main_tick = tick

except OSError:
    pass

# --- Final summary ---
print("\n" + "=" * 60)
bench_name = os.path.splitext(os.path.basename(args.firmware))[0]
print(f"Benchmark : {bench_name}")

ticks_per_cycle = TICK_PER_SEC // CLK_FREQ_HZ

if roi_start_tick is not None and roi_end_tick is not None:
    delta = roi_end_tick - roi_start_tick
    cycles = delta / ticks_per_cycle
    print(f"ROI Start : {roi_start_tick}")
    print(f"ROI End   : {roi_end_tick}")
    print(f"Delta     : {delta} ticks")
    print(f"Cycles    : {cycles:.1f}  (at {CLK_FREQ_HZ / 1e6:.0f} MHz)")

    print(
        f"  [debug] precise_start={precise_start_tick}, precise_end={precise_end_tick}"
    )
    if precise_start_tick and precise_end_tick:
        precise_delta = precise_end_tick - precise_start_tick
        precise_cycles = precise_delta / ticks_per_cycle
        print(f"")
        print(f"Precise (kernel only, excluding m5op overhead):")
        print(f"  First kernel inst : {precise_start_tick}")
        print(f"  Last kernel inst  : {precise_end_tick}")
        print(f"  Delta             : {precise_delta} ticks")
        print(f"  Cycles            : {precise_cycles:.1f}")
else:
    missing = []
    if roi_start_tick is None:
        missing.append("work_begin")
    if roi_end_tick is None:
        missing.append("work_end")
    print(f"ROI INCOMPLETE: missing {', '.join(missing)}")

print("=" * 60)
