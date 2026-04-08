#!/usr/bin/env python3
"""Simple gem5 runner for bare-metal VFP test on STM32G474RE board."""

import argparse
import os
import sys

import m5
from m5.objects import Root
from m5.objects.ArmSemihosting import ArmSemihosting

from gem5.prebuilt.cortexm.boards import STM32G474RETimingBoard

parser = argparse.ArgumentParser()
parser.add_argument("--firmware", required=True)
parser.add_argument("--tick-limit", type=int, default=10_000_000_000)
args = parser.parse_args()

board = STM32G474RETimingBoard(enable_art=False)
board.semihosting = ArmSemihosting()
board.set_workload(args.firmware)

root = Root(full_system=True, system=board)
m5.instantiate()

print("Running...")
exit_event = m5.simulate(args.tick_limit)
cause = exit_event.getCause()
print(f"Exited: {cause} @ tick {m5.curTick()}")

if "Stopped" in cause or "exit" in cause.lower():
    print("PASS")
else:
    print(f"RESULT: {cause}")
