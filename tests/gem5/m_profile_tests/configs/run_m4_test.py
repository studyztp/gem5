# Copyright (c) 2026 University of California, Davis and Cornell University
# All rights reserved.
#
# gem5 config script for running Cortex-M4 test programs.
#
# Usage:
#   ./build/ALL/gem5.opt -re \
#       tests/gem5/m_profile_tests/configs/run_m4_test.py \
#       --firmware tests/gem5/m_profile_tests/programs/cortexm4_basic.elf \
#       [--tick-limit 1000000] [--cpu-type atomic|timing|minor]
#
# The -re flag redirects all gem5 output (including Exec debug lines) to
# m5out/simout.txt.  This script enables the Exec flag programmatically so
# the caller does not need to pass --debug-flags=Exec separately.
#
# This script:
#   1. Creates an STM32F405 platform
#   2. Creates a CPU (Atomic, Timing, or Minor based on --cpu-type)
#   3. Wires everything via ArmMBoard
#   4. Loads the firmware ELF
#   5. Enables gem5 Exec debug flag (logs every instruction + memory op)
#   6. Runs for the specified tick limit (default 1M ticks)
#   7. Parses m5out/simout.txt for the last store to 0x20000100 and exits
#
# Exit codes:
#   0 = actual result matches --expected-result (default: pass)
#   1 = mismatch or inconclusive
#
# Test result protocol (firmware side):
#   [0x20000100] = 0xCAFECAFE → all subtests passed
#   [0x20000100] = 0xDEADDEAD → at least one subtest failed
#   [0x20000104] = subtest number of first failure (1-based)
#   [0x20000108] = optional diagnostic value
#
# How the test result is read:
#   gem5's Exec debug flag emits one line per instruction, including stores:
#     TICK: system.cpu: T0 : 0xPC : str r0, [r1] : MemWrite : D=0xVALUE A=0xADDR
#   We scan m5out/simout.txt for the LAST store to 0x20000100.  The last
#   write is authoritative: firmware may write intermediate values before
#   the final PASS or FAIL.

import argparse
import os
import re
import sys

import m5
from m5 import debug as m5_debug
from m5.objects import *
from m5.objects.ArmSemihosting import ArmSemihosting
from m5.params import AddrRange

# ---------------------------------------------------------------------------
# Test result magic values (firmware protocol)
# ---------------------------------------------------------------------------
TEST_RESULT_ADDR = 0x20000100
TEST_SUBTEST_ADDR = 0x20000104
TEST_PASS_VAL = 0xCAFECAFE
TEST_FAIL_VAL = 0xDEADDEAD

# ---------------------------------------------------------------------------
# Parse arguments
# ---------------------------------------------------------------------------
parser = argparse.ArgumentParser(description="Run Cortex-M4 test on gem5")
parser.add_argument(
    "--firmware", required=True, help="Path to Cortex-M4 ELF binary"
)
parser.add_argument(
    "--tick-limit",
    type=int,
    default=0,
    help="Maximum simulation ticks (0 = no limit, default)",
)
parser.add_argument(
    "--cpu-type",
    choices=["atomic", "timing", "minor"],
    default="atomic",
    help="CPU model: atomic (default), timing, or minor",
)
parser.add_argument(
    "--expected-result",
    choices=["pass", "fail"],
    default="pass",
    help="Expected test outcome: pass (default) or fail",
)
args = parser.parse_args()

# ---------------------------------------------------------------------------
# Platform — STM32F405: flash at 0x08000000, SRAM1 at 0x20000000
# ---------------------------------------------------------------------------
from gem5.prebuilt.cortexm.platforms import STM32F405Platform

platform = STM32F405Platform()

# ---------------------------------------------------------------------------
# CPU
# ---------------------------------------------------------------------------
if args.cpu_type == "timing":
    cpu = ArmMTimingSimpleCPU()
elif args.cpu_type == "minor":
    cpu = ArmMMinorCPU()
else:
    cpu = ArmMAtomicSimpleCPU()

# ---------------------------------------------------------------------------
# Memories — flash + 3 SRAM blocks
# ---------------------------------------------------------------------------
memories = platform.default_memories()

# ---------------------------------------------------------------------------
# Board
# ---------------------------------------------------------------------------
from gem5.components.boards.arm_m_board import ArmMBoard

board = ArmMBoard(
    platform=platform,
    cpu=cpu,
    memories=memories,
    clk_freq="168MHz",
)

# ---------------------------------------------------------------------------
# Semihosting — allows firmware to exit cleanly via BKPT #0xAB
# ---------------------------------------------------------------------------
board.semihosting = ArmSemihosting()

# ---------------------------------------------------------------------------
# Workload
# ---------------------------------------------------------------------------
board.set_workload(args.firmware)

# ---------------------------------------------------------------------------
# Root
# ---------------------------------------------------------------------------
root = Root(full_system=True, system=board)

# ---------------------------------------------------------------------------
# Enable Exec debug flag before m5.instantiate().
# This logs every instruction with effective address and data for stores.
# gem5 writes debug output to m5out/simout.txt when invoked with -re.
# ---------------------------------------------------------------------------
m5_debug.flags["Exec"].enable()

# ---------------------------------------------------------------------------
# Instantiate and run
# ---------------------------------------------------------------------------
m5.instantiate()

print(
    f"Starting simulation: firmware={args.firmware}, "
    f"tick_limit={'unlimited' if args.tick_limit == 0 else args.tick_limit}"
)

# Run until semihosting exit, crash, or optional tick limit.
# All firmware exits via BKPT #0xAB (semihosting SYS_EXIT), so
# the tick limit is only needed as a manual safety net.
if args.tick_limit > 0:
    exit_event = m5.simulate(args.tick_limit)
else:
    exit_event = m5.simulate()

print(f"Exiting @ tick {m5.curTick()} because {exit_event.getCause()}")

# ---------------------------------------------------------------------------
# Parse m5out/simout.txt for the test result
#
# Exec trace format for a store:
#   TICK: system.cpu: T0 : 0xPC : str r0,[r1] : MemWrite : D=0xVALUE A=0xADDR
#
# We look for D=0x<val> A=0x<addr> pairs.  The 32-bit mask handles the case
# where gem5 prints 64-bit values (e.g. D=0x0000000000cafecafe).
# ---------------------------------------------------------------------------
_RE_STORE = re.compile(r"D=0x([0-9a-fA-F]+)\s+A=0x([0-9a-fA-F]+)")


def parse_simout():
    """Return (result, subtest) from the last stores to the protocol addrs.

    Returns (None, None) if the trace file is missing or no matching store
    was found.
    """
    result = None
    subtest = None
    try:
        # Use m5.options.outdir so this works regardless of the -d flag.
        # Hardcoding "m5out/simout.txt" silently reads a stale file when
        # gem5 is invoked with -d <dir>.
        simout_path = os.path.join(m5.options.outdir, "simout.txt")
        with open(simout_path) as f:
            for line in f:
                m = _RE_STORE.search(line)
                if m is None:
                    continue
                value = int(m.group(1), 16) & 0xFFFFFFFF
                addr = int(m.group(2), 16)
                if addr == TEST_RESULT_ADDR:
                    result = value
                elif addr == TEST_SUBTEST_ADDR:
                    subtest = value
    except OSError as exc:
        print(f"  Warning: could not open {simout_path}: {exc}")
    return result, subtest


result, subtest = parse_simout()

# ---------------------------------------------------------------------------
# Report and exit
# ---------------------------------------------------------------------------
if result is None:
    print("TEST INCONCLUSIVE: no store to 0x20000100 seen in simout.txt")
    print("  Make sure to run gem5 with -re so Exec output goes to simout.txt")
    sys.exit(1)

elif result == TEST_PASS_VAL:
    print("TEST PASSED")
    sys.exit(0 if args.expected_result == "pass" else 1)

elif result == TEST_FAIL_VAL:
    print(f"TEST FAILED at subtest {subtest}")
    sys.exit(0 if args.expected_result == "fail" else 1)

else:
    print(
        f"TEST INCONCLUSIVE: result=0x{result:08X} "
        f"(expected 0x{TEST_PASS_VAL:08X} or 0x{TEST_FAIL_VAL:08X})"
    )
    sys.exit(1)
