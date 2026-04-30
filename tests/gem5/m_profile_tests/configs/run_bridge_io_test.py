# Copyright (c) 2026 University of California, Davis and Cornell University
# All rights reserved.
#
# Smoke test for MProfileBridgeIO on STM32G474RETimingBoard.
#
# Usage:
#   ./build/ALL/gem5.opt -re \
#       tests/gem5/m_profile_tests/configs/run_bridge_io_test.py \
#       --firmware tests/gem5/m_profile_tests/programs/test_bridge_io.elf
#
# Verifies BOTH:
#   1. Standard pass marker — last store of 0xCAFECAFE to 0x20000100 in
#      the Exec trace (same protocol as run_timing_board_test.py).
#   2. Bridge done message — "MProfileBridgeIO signaled done." appears
#      in the gem5 simout, confirming exitSimLoopNow was triggered by
#      the firmware's write to register[1].
#
# A test passes only when both checks succeed.  This catches:
#   - The CPU never reached the success path → no PASS store.
#   - The CPU reached the PASS store but the bridge done write didn't
#     trigger exitSimLoopNow (the firmware's belt-and-braces semihosted
#     SYS_EXIT will end the sim, the PASS store will be present, but
#     the done message won't be).

import argparse
import os
import re
import sys

import m5
from m5 import debug as m5_debug
from m5.objects import Root
from m5.objects.ArmSemihosting import ArmSemihosting
from m5.objects.MProfileBridgeIO import MProfileBridgeIO

from gem5.prebuilt.cortexm.boards import STM32G474RETimingBoard

parser = argparse.ArgumentParser()
parser.add_argument("--firmware", required=True)
parser.add_argument("--tick-limit", type=int, default=0)
# Highest valid IRQ index for STM32G474RE (num_irqs=102 → 0..101).
parser.add_argument("--bridge-irq-num", type=int, default=101)
# Bridge MMIO base.  0x90000000 is in the STM32G4 unused
# "external memory/device" region (RM0440 §2.2) — outside flash,
# SRAM, peripherals, and the SCS.
parser.add_argument(
    "--bridge-pio-addr", type=lambda s: int(s, 0), default=0x90000000
)
# Required by the m_profile_test() helper in test_m_profile.py — accepted
# but ignored here, since this test reports its own pass/fail.
parser.add_argument("--expected-result", default="pass")
args = parser.parse_args()

board = STM32G474RETimingBoard()
board.semihosting = ArmSemihosting()
board.set_workload(args.firmware)

# Attach the bridge externally (not via a board kwarg) so this test
# config controls every parameter — buffer sizes, address, IRQ — and
# matches the idiomatic gem5 attachment style used elsewhere (e.g.
# `board.semihosting = ArmSemihosting()` above).  The board exposes
# `platform.scs` and `system_bus.mem_side_ports` for exactly this.
board.bridge_io = MProfileBridgeIO(
    pio_addr=args.bridge_pio_addr,
    scs=board.platform.scs,
    irq_num=args.bridge_irq_num,
)
board.bridge_io.pio = board.system_bus.mem_side_ports

root = Root(full_system=True, system=board)
# Exec trace is needed by the simout-store scan below.  Existing tests
# (run_m4_test.py, run_timing_board_test.py) enable it the same way.
m5_debug.flags["Exec"].enable()

m5.instantiate()
print(f"Board constructed successfully, starting simulation...")

if args.tick_limit > 0:
    exit_event = m5.simulate(args.tick_limit)
else:
    exit_event = m5.simulate()

exit_cause = exit_event.getCause()
print(f"Exiting @ tick {m5.curTick()} because {exit_cause}")

# ---------------------------------------------------------------------------
# Check 1: pass marker store
# ---------------------------------------------------------------------------
TEST_RESULT_ADDR = 0x20000100
TEST_PASS_VAL = 0xCAFECAFE
_RE_STORE = re.compile(r"D=0x([0-9a-fA-F]+)\s+A=0x([0-9a-fA-F]+)")
# String the bridge prints when registers[1] is written to 1.  Source
# of truth: src/dev/arm/m_profile_bridge_io.cc — keep this in sync.
BRIDGE_DONE_MSG = "MProfileBridgeIO signaled done."

result = None
done_seen = False
try:
    simout_path = os.path.join(m5.options.outdir, "simout.txt")
    with open(simout_path) as f:
        for line in f:
            # Run both regex matches per line — simout is a single
            # pass and we want to capture the last PASS-marker store
            # AND any sighting of the bridge done message.
            m_obj = _RE_STORE.search(line)
            if m_obj is not None:
                addr = int(m_obj.group(2), 16)
                if addr == TEST_RESULT_ADDR:
                    result = int(m_obj.group(1), 16) & 0xFFFFFFFF
            if BRIDGE_DONE_MSG in line:
                done_seen = True
except OSError:
    pass

# Some gem5 builds print the exit cause to stdout/stderr instead of the
# in-tree simout — also check the exit_cause string we got from the
# simulate() return so the test can pass without scraping a file.
if BRIDGE_DONE_MSG in exit_cause:
    done_seen = True

# ---------------------------------------------------------------------------
# Decision
# ---------------------------------------------------------------------------
if result == TEST_PASS_VAL and done_seen:
    print("TEST PASSED")
    sys.exit(0)

# Below: differentiate the two "not pass" failure modes so the user can
# tell whether the firmware succeeded but the bridge done path is broken,
# or vice versa.
if result == TEST_PASS_VAL and not done_seen:
    print(
        "TEST FAILED: PASS marker present but bridge 'done' message "
        "not seen — exitSimLoopNow path may be broken"
    )
    sys.exit(1)
if result is not None and result != TEST_PASS_VAL:
    print(f"TEST FAILED: result=0x{result:08X} (firmware reported failure)")
    sys.exit(1)
print(
    "TEST INCONCLUSIVE: no PASS marker store seen " f"(done_seen={done_seen})"
)
sys.exit(1)
