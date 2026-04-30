# Copyright (c) 2026 University of California, Davis and Cornell University
# All rights reserved.
#
# IRQ-driven echo test for MProfileBridgeIO on STM32G474RETimingBoard.
#
# Mirrors the ping-pong pattern of:
#   https://github.com/studyztp/crazyflie/blob/main/gem5-script/gem5-crazyflie-script.py
#
# Each iteration:
#   1. Python writes input bytes via updateInputData().
#   2. Python wakes firmware via raiseInterrupt().
#   3. simulate() runs until firmware writes registers[1] = 1, which
#      makes the bridge call exitSimLoopNow with the message
#      "MProfileBridgeIO signaled done.".
#   4. Python reads getOutputData()[:getOutputDataSize()] and verifies
#      it matches the input (echo semantics).
#   5. Python clears the IRQ via clearInterrupt() — exercises the
#      clear path even though the handler already drained pending.
#
# Iteration 0 is special: the firmware boots, writes the sentinel
# "INIT" to the output buffer, and signals done before any IRQ.  This
# exercises the boot path independently of the IRQ machinery.
#
# Pass criterion: every iteration's output matches expectations.  On
# the first mismatch we print a diagnostic and exit non-zero; on full
# success we print "TEST PASSED" and exit 0.

import argparse
import os
import sys

import m5
from m5.objects import Root
from m5.objects.ArmSemihosting import ArmSemihosting
from m5.objects.MProfileBridgeIO import MProfileBridgeIO

from gem5.prebuilt.cortexm.boards import STM32G474RETimingBoard

# String the bridge prints when registers[1] is written to 1.
# Source of truth: src/dev/arm/m_profile_bridge_io.cc.
DONE_MSG = "MProfileBridgeIO signaled done."

# 10 ms per iteration is many orders of magnitude more than the
# handler needs (a few hundred cycles at 170 MHz).  Used as a
# runaway guard — if this elapses without a "done" message, the
# firmware is stuck or crashed.
DEFAULT_PER_ITER_TICK_LIMIT = 10**10


parser = argparse.ArgumentParser(
    description="IRQ-driven echo test for MProfileBridgeIO."
)
parser.add_argument("--firmware", required=True)
# Bridge MMIO base.  Default 0x90000000 matches test_bridge_io_irq.S's
# BRIDGE_BASE constant — change them together if you remap.
parser.add_argument(
    "--bridge-pio-addr", type=lambda s: int(s, 0), default=0x90000000
)
# Highest valid IRQ index for STM32G474RE (num_irqs=102 → 0..101).
# The firmware's vector table places its bridge handler at IRQ 101.
parser.add_argument("--bridge-irq-num", type=int, default=101)
parser.add_argument(
    "--per-iter-tick-limit", type=int, default=DEFAULT_PER_ITER_TICK_LIMIT
)
# Required by the m_profile_test() helper.  Accepted but ignored —
# this script reports its own pass/fail.
parser.add_argument("--expected-result", default="pass")
args = parser.parse_args()

# ---------------------------------------------------------------------------
# Board + bridge wiring
# ---------------------------------------------------------------------------
board = STM32G474RETimingBoard()
board.semihosting = ArmSemihosting()
board.set_workload(args.firmware)

# Caller-side bridge attachment (Round 6 idiom).  Using `board.platform.scs`
# and `board.system_bus.mem_side_ports` — both already exposed by the
# board.  See the comment in stm32g474re_board.py near the SCS wiring.
board.bridge_io = MProfileBridgeIO(
    pio_addr=args.bridge_pio_addr,
    scs=board.platform.scs,
    irq_num=args.bridge_irq_num,
)
board.bridge_io.pio = board.system_bus.mem_side_ports

root = Root(full_system=True, system=board)
m5.instantiate()
print("[bridge_io_irq] Board constructed; starting boot.")


# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------
def expect_done(label):
    """Run simulate() and assert it exited via the bridge done message."""
    e = m5.simulate(args.per_iter_tick_limit)
    cause = e.getCause()
    if DONE_MSG not in cause:
        # Either the tick budget elapsed (firmware hung), or the CPU
        # exited via some other path (fault, semihosting SYS_EXIT).
        # Surface the cause string so the user can debug.
        print(f"FAIL [{label}]: expected '{DONE_MSG}', got '{cause}'")
        sys.exit(1)
    print(f"[{label}] sim exit @ tick {m5.curTick()}: {cause}")


def read_output():
    """Return the valid prefix of the output buffer as bytes."""
    raw = board.bridge_io.getOutputData()
    n = board.bridge_io.getOutputDataSize()
    # `raw` is a pybind-bound std::vector<uint8_t>.  Slicing-then-bytes()
    # works whether pybind exposes it as list[int] or as a sequence of
    # ints — both convert via the bytes() constructor.
    return bytes(list(raw)[:n])


def fail(label, expected, got):
    print(f"FAIL [{label}]: expected {expected!r}, got {got!r}")
    sys.exit(1)


# ---------------------------------------------------------------------------
# Iteration 0 — boot.  Firmware writes "INIT" before any IRQ machinery.
# ---------------------------------------------------------------------------
expect_done("boot")
out = read_output()
if out != b"INIT":
    fail("boot", b"INIT", out)
print(f"[boot] output OK: {out!r}")

# ---------------------------------------------------------------------------
# Iterations 1..N — Python ping-pong with the firmware via IRQs.
#
# Inputs picked to exercise: short ASCII, longer ASCII, and a binary
# payload with the high bit set.  All within the 1 KiB input buffer.
# ---------------------------------------------------------------------------
inputs = [
    b"hi!",
    b"hello world",
    bytes([0xDE, 0xAD, 0xBE, 0xEF, 0x12, 0x34, 0x56, 0x78]),
]

for i, data in enumerate(inputs, start=1):
    label = f"iter {i}"

    # Reset done flag for hygiene (firmware will set it back to 1).
    # Functionally optional — exitSimLoopNow re-fires on every write
    # of 1 — but it lets a Python reader of registers[1] distinguish
    # "iteration in progress" from "iteration complete".
    board.bridge_io.updateDone(False)

    if not board.bridge_io.updateInputData(list(data)):
        # Returns False if the device is off (go=0) or the data
        # exceeds input_data_buffer_size.  Neither should happen here.
        fail(label, "updateInputData accepted", "updateInputData rejected")

    if not board.bridge_io.raiseInterrupt():
        # Returns False if the device is off (go=0).  Shouldn't happen
        # under default construction, but check explicitly so a
        # regression here gets a clear failure message.
        fail(
            label, "raiseInterrupt accepted", "raiseInterrupt rejected (go=0?)"
        )

    expect_done(label)

    # Exercise the clear path.  No-op functionally because the IRQ
    # was already drained by the handler entry, but it shouldn't
    # crash and it's part of the API the crazyflie script uses.
    board.bridge_io.clearInterrupt()

    out = read_output()
    if out != data:
        fail(label, data, out)
    print(f"[{label}] echoed OK: {out!r}")


# ---------------------------------------------------------------------------
# All iterations passed.
# ---------------------------------------------------------------------------
print("TEST PASSED")
sys.exit(0)
