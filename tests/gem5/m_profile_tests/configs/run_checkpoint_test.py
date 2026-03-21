# Copyright (c) 2026 University of California, Davis and Cornell University
# All rights reserved.
#
# Two-phase checkpoint/restore test for M-profile SCS (Bug 19).
#
# Uses multiprocessing.Process to run two separate gem5 simulations
# in the same script (gem5 cannot be re-initialized within one process).
#
# Phase 1 (child process): Run firmware that configures NVIC priorities,
#   enables IRQ0-3, and triggers nested exceptions.  Checkpoint at tick
#   523776 — inside C_Handler after pending A and D, before A preempts.
#   State at checkpoint: C active, A+D pending.
#
# Phase 2 (child process): Restore from checkpoint.  Firmware resumes
#   in C_Handler.  If SCS state survived, the full nesting chain plays
#   out (A preempts C, B fires after A, C resumes, D fires last).
#   Firmware verifies execution order and exits via semihosting.
#
# Usage:
#   ./build/ALL/gem5.opt -re \
#       tests/gem5/m_profile_tests/configs/run_checkpoint_test.py \
#       --firmware tests/gem5/m_profile_tests/programs/test_scs_checkpoint.elf

import argparse
import os
import re
import sys
from multiprocessing import Process

import m5
from m5 import debug as m5_debug
from m5.objects import (
    ArmMAtomicSimpleCPU,
    Root,
)
from m5.objects.ArmSemihosting import ArmSemihosting

from gem5.components.boards.arm_m_board import ArmMBoard
from gem5.prebuilt.cortexm.platforms import STM32F405Platform

# Checkpoint tick — determined from nested_priority Exec trace.
# At tick 523776, C_Handler has just written to ISPR0 to pend A (IRQ0).
# The DSB+ISB that triggers A's preemption hasn't executed yet.
# State: C (IRQ2) active, A (IRQ0) + D (IRQ3) pending.
# gem5 simulation is deterministic so this tick is reproducible.
CHECKPOINT_TICK = 523776

# Exit codes for child processes
_EXIT_OK = 0
_EXIT_FAIL = 1
_EXIT_CHECKPOINT = 42


def _build_system(firmware_path):
    """Build the M-profile system (shared by both phases)."""
    platform = STM32F405Platform()
    cpu = ArmMAtomicSimpleCPU()
    memories = platform.default_memories()

    board = ArmMBoard(
        platform=platform,
        cpu=cpu,
        memories=memories,
        clk_freq="168MHz",
    )
    board.semihosting = ArmSemihosting()
    board.set_workload(firmware_path)

    root = Root(full_system=True, system=board)
    return root


def _run_phase1(firmware_path, cpt_path):
    """Phase 1: run firmware to the checkpoint tick, then checkpoint.

    By tick 523776 the firmware has:
      - Configured IRQ priorities (IPR0 = 0x80804000)
      - Enabled IRQ0-3 (ISER0 = 0x0F)
      - Entered C_Handler (IRQ2, priority 0x80)
      - Recorded C_START marker
      - Pended D (IRQ3, equal priority, stays pending)
      - Pended A (IRQ0, higher priority, about to preempt)
    The checkpoint captures this mid-handler state.
    """
    root = _build_system(firmware_path)
    m5_debug.flags["Exec"].enable()
    m5.instantiate()

    exit_event = m5.simulate(CHECKPOINT_TICK)
    cause = exit_event.getCause()

    if "simulate() limit reached" not in cause:
        print(
            f"Phase 1: Unexpected exit at tick {m5.curTick()}: {cause}",
            file=sys.stderr,
        )
        sys.exit(_EXIT_FAIL)

    m5.checkpoint(cpt_path)
    print(
        f"Phase 1: Checkpoint saved at tick {m5.curTick()} to {cpt_path}",
        file=sys.stderr,
    )
    sys.exit(_EXIT_CHECKPOINT)


def _run_phase2(firmware_path, cpt_path):
    """Phase 2: restore from checkpoint and run to completion.

    On restore, firmware resumes in C_Handler.  The full exception
    nesting chain should play out:
      C continues → A preempts → A pends B → A returns →
      B preempts C → B returns → C resumes → C returns →
      D runs → D returns → Thread mode verifies order.

    If Bug 19 exists (SCS not serialized), pending/active/priority
    state is lost → chain breaks → firmware reports FAIL.
    """
    root = _build_system(firmware_path)
    m5_debug.flags["Exec"].enable()

    m5.instantiate(cpt_path)
    print(f"Phase 2: Restored from {cpt_path}", file=sys.stderr)

    # The full exception chain + verification takes ~2M ticks.
    # 10M ticks is plenty of headroom.
    exit_event = m5.simulate(10_000_000)
    cause = exit_event.getCause()
    print(f"Exiting @ tick {m5.curTick()} because {cause}")

    # Parse PASS/FAIL from Exec trace (stores to TEST_RESULT/TEST_SUBTEST)
    _RE_STORE = re.compile(r"D=0x([0-9a-fA-F]+)\s+A=0x([0-9a-fA-F]+)")

    result = None
    subtest = None
    simout_path = os.path.join(m5.options.outdir, "simout.txt")
    try:
        with open(simout_path) as f:
            for line in f:
                m_match = _RE_STORE.search(line)
                if m_match is None:
                    continue
                value = int(m_match.group(1), 16) & 0xFFFFFFFF
                addr = int(m_match.group(2), 16)
                if addr == 0x20000100:  # TEST_RESULT
                    result = value
                elif addr == 0x20000104:  # TEST_SUBTEST
                    subtest = value
    except OSError:
        pass

    if result == 0xCAFECAFE:
        print("TEST PASSED")
        sys.exit(_EXIT_OK)
    elif result == 0xDEADDEAD:
        print(f"TEST FAILED at subtest {subtest}")
        sys.exit(_EXIT_FAIL)
    else:
        print("TEST INCONCLUSIVE: no result found")
        sys.exit(_EXIT_FAIL)


# ---------------------------------------------------------------------------
# Parent process: parse args and orchestrate child processes
# ---------------------------------------------------------------------------
parser = argparse.ArgumentParser(
    description="Checkpoint/restore test for M-profile SCS"
)
parser.add_argument(
    "--firmware", required=True, help="Path to test_scs_checkpoint.elf"
)
args = parser.parse_args()

cpt_path = os.path.join(m5.options.outdir, "scs_test.cpt")

# Phase 1: run firmware to checkpoint tick, save checkpoint
print("=== Phase 1: Configure state and checkpoint ===")
p1 = Process(target=_run_phase1, args=(args.firmware, cpt_path))
p1.start()
p1.join()

if p1.exitcode != _EXIT_CHECKPOINT:
    print(f"Phase 1 failed (exit code {p1.exitcode})")
    print("TEST FAILED at subtest 0")
    sys.exit(1)

# Phase 2: restore from checkpoint, firmware verifies exception chain
print("=== Phase 2: Restore and verify ===")
p2 = Process(target=_run_phase2, args=(args.firmware, cpt_path))
p2.start()
p2.join()

if p2.exitcode == _EXIT_OK:
    print("TEST PASSED")
    sys.exit(0)
else:
    sys.exit(1)
