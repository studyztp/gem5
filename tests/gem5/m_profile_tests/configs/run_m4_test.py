# Copyright (c) 2026 University of California, Davis and Cornell University
# All rights reserved.
#
# gem5 config script for running Cortex-M4 test programs.
#
# Usage:
#   ./build/ARM/gem5.opt [--debug-flags=Exec,MProfileCPSR] \
#       tests/gem5/m_profile_tests/configs/run_m4_test.py \
#       --firmware tests/gem5/m_profile_tests/programs/cortexm4_basic.elf \
#       [--tick-limit 1000000]
#
# This script:
#   1. Creates an STM32F405 platform
#   2. Creates an ArmMAtomicSimpleCPU
#   3. Wires everything via ArmMBoard
#   4. Loads the firmware ELF
#   5. Runs for the specified tick limit (default 1M ticks)
#
# Validation:
#   After simulation, check m5out/stats.txt and the debug trace.
#   The test firmware writes results to SRAM at 0x20000100:
#     [0x20000100] = 0xCAFECAFE if SVCall test passed
#     [0x20000104] = 0xCAFECAFE if SysTick test passed
#     [0x20000108] = 0xCAFECAFE if all tests passed
#                  = 0xDEADDEAD if any test failed

import argparse
import sys

import m5
from m5.objects import *
from m5.params import AddrRange

# Parse arguments
parser = argparse.ArgumentParser(description="Run Cortex-M4 test on gem5")
parser.add_argument(
    "--firmware", required=True, help="Path to Cortex-M4 ELF binary"
)
parser.add_argument(
    "--tick-limit",
    type=int,
    default=1000000,
    help="Maximum simulation ticks (default 1M)",
)
args = parser.parse_args()

# --- Platform ---
# STM32F405 memory map: flash at 0x08000000, SRAM at 0x20000000
from gem5.prebuilt.cortexm.platforms import STM32F405Platform

platform = STM32F405Platform()

# --- CPU ---
cpu = ArmMAtomicSimpleCPU()

# --- Memories ---
# Use the platform's default memories (flash + 3 SRAM blocks)
memories = platform.default_memories()

# --- Board ---
from gem5.components.boards.arm_m_board import ArmMBoard

board = ArmMBoard(
    platform=platform,
    cpu=cpu,
    memories=memories,
    clk_freq="168MHz",
)

# --- Workload ---
board.set_workload(args.firmware)

# --- Root ---
root = Root(full_system=True, system=board)

# --- Instantiate and run ---
m5.instantiate()

print(
    f"Starting simulation: firmware={args.firmware}, "
    f"tick_limit={args.tick_limit}"
)

exit_event = m5.simulate(args.tick_limit)

print(f"Exiting @ tick {m5.curTick()} because {exit_event.getCause()}")

# --- Read test results from SRAM ---
# The firmware writes results to 0x20000100
TEST_RESULTS = 0x20000100
TEST_PASS_VAL = 0xCAFECAFE
TEST_FAIL_VAL = 0xDEADDEAD

try:
    # Read test results via physical proxy
    proxy = board.system_port
    # Note: reading memory after simulation requires the physical proxy.
    # This may not work in all configurations.  For validation, use
    # --debug-flags=Exec to trace instruction execution instead.
    pass
except Exception as e:
    print(f"Could not read test results: {e}")
    print("Use --debug-flags=Exec,MProfileCPSR to trace execution")
