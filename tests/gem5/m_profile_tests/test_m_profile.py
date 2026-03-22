# Copyright (c) 2026 University of California, Davis and Cornell University
# All rights reserved.
#
# Redistribution and use in source and binary forms, with or without
# modification, are permitted provided that the following conditions are
# met: redistributions of source code must retain the above copyright
# notice, this list of conditions and the following disclaimer;
# redistributions in binary form must reproduce the above copyright
# notice, this list of conditions and the following disclaimer in the
# documentation and/or other materials provided with the distribution;
# neither the name of the copyright holders nor the names of its
# contributors may be used to endorse or promote products derived from
# this software without specific prior written permission.
#
# THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
# "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
# LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
# A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
# OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
# SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
# LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
# DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
# THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
# (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
# OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

"""
M-profile integration tests for gem5.

Each test runs a specific Cortex-M4 firmware ELF that exercises a
particular feature (CPS, MRS/MSR, load/store, BX/EXC_RETURN, etc.).
The firmware writes 0xCAFECAFE to 0x20000108 on success or
0xDEADDEAD on failure.

The test verifies that:
1. gem5 doesn't crash (non-zero exit = fail)
2. The simulation reaches the test_done loop (tick limit exit)

To validate specific instruction behavior, run with:
  --debug-flags=Exec,MProfileCPSR

These tests require pre-built ELF firmware.  Build with:
  cd tests/gem5/m_profile_tests/programs && make
"""

import os
import re

from testlib import *


def m_profile_test(
    name, firmware_name, expected_result="pass", extra_args=None
):
    """Register an M-profile integration test.

    Args:
        name: Test name (e.g., "test_cps")
        firmware_name: ELF filename (e.g., "test_cps.elf")
        expected_result: "pass" (default) or "fail" — what the firmware
            is expected to report.  Controls --expected-result flag
            passed to run_m4_test.py.
        extra_args: Optional list of additional CLI args for run_m4_test.py
            (e.g., ["--num-irqs", "240"]).
    """
    firmware_path = joinpath(
        config.base_dir,
        "tests",
        "gem5",
        "m_profile_tests",
        "programs",
        firmware_name,
    )

    # Skip if firmware not built
    if not os.path.exists(firmware_path):
        return

    # Verifier: check that simulation completed without panic/crash.
    # The simulation exits either via semihosting SYS_EXIT (bkpt #0xab)
    # or by hitting the tick limit.  Both are valid exits.
    verifiers = [
        verifier.MatchRegex(re.compile(r"Exiting @ tick \d+ because")),
    ]

    config_args = [
        "--firmware",
        firmware_path,
        "--expected-result",
        expected_result,
    ]
    if extra_args:
        config_args.extend(extra_args)

    gem5_verify_config(
        name=f"m_profile_{name}",
        verifiers=verifiers,
        fixtures=(),
        config=joinpath(
            config.base_dir,
            "tests",
            "gem5",
            "m_profile_tests",
            "configs",
            "run_m4_test.py",
        ),
        config_args=config_args,
        valid_isas=(constants.all_compiled_tag,),
        valid_hosts=constants.supported_hosts,
        length=constants.quick_tag,
    )


# Register all test firmware
#
# Each test isolates a specific M-profile feature:
m_profile_test("basic", "cortexm4_basic.elf")
m_profile_test("cps", "test_cps.elf")
m_profile_test("mrs_msr", "test_mrs_msr.elf")
m_profile_test("bx_exc_return", "test_bx_exc_return.elf")
m_profile_test("load_store", "test_load_store.elf")
m_profile_test("blocked_insts", "test_blocked_insts.elf")
m_profile_test("sp_sync", "test_sp_sync.elf")
m_profile_test("pop_pc_exc_return", "test_pop_pc_exc_return.elf")
m_profile_test("nested_exceptions", "test_nested_exceptions.elf")
m_profile_test("data_processing", "test_data_processing.elf")
m_profile_test("psp_exception", "test_psp_exception.elf")
m_profile_test(
    "deliberate_fail", "test_deliberate_fail.elf", expected_result="fail"
)
m_profile_test("exc_return_mask", "test_exc_return_mask.elf")
m_profile_test("blx_no_exc_return", "test_blx_no_exc_return.elf")
m_profile_test("bx_bit0_fault", "test_bx_bit0_fault.elf")
m_profile_test("srs_rfe_blocked", "test_srs_rfe_blocked.elf")
m_profile_test("coproc_detection", "test_coproc_detection.elf")
m_profile_test("ldrex_strex_granule", "test_ldrex_strex_granule.elf")
m_profile_test("nested_priority", "test_nested_priority.elf")
m_profile_test("systick_active_pending", "test_systick_active_pending.elf")
m_profile_test("ipr_byte_access", "test_ipr_byte_access.elf")
# BUG-1: NVIC array OOB — 240-IRQ config tests corruption detection (Phase A),
# high-IRQ register read/write (Phase B), and nested preemption (Phase C).
# Requires num_irqs=240 because Phase C delivers IRQs 224 and 239 which
# need updatePending() to scan word 7 of the NVIC arrays.
m_profile_test(
    "nvic_oob_240", "test_nvic_oob.elf", extra_args=["--num-irqs", "240"]
)
# BUG-2: ICSR dynamic field read — basic static check with SVCall handler
m_profile_test("icsr_read", "test_icsr_read.elf")
# BUG-3: CPUID read-only enforcement
m_profile_test("cpuid_readonly", "test_cpuid_readonly.elf")
# BUG-4: NVIC unimplemented IRQ bit masking (RAZ/WI)
m_profile_test("nvic_irq_mask", "test_nvic_irq_mask.elf")
# BUG-5: CC flat reg ↔ xPSR NZCV sync
m_profile_test("xpsr_nzcv_sync", "test_xpsr_nzcv_sync.elf")
# BUG-6: CONTROL.SPSEL write swaps R13 between MSP/PSP
m_profile_test("control_spsel", "test_control_spsel.elf")
# BUG-7: SCB sub-word (byte/halfword) access
m_profile_test("scb_byte_access", "test_scb_byte_access.elf")
# MISSING-1: Invalid EXC_RETURN validation
m_profile_test("exc_return_validate", "test_exc_return_validate.elf")
# C-1: FAULTMASK priority — executionPriority() returns uint8_t, thread-mode
# default is 0xFF instead of 256.  With 8 priority bits, IRQ at priority 0xFF
# cannot be delivered from Thread mode (0xFF < 0xFF = false).
m_profile_test(
    "faultmask_priority",
    "test_faultmask_priority.elf",
    extra_args=["--priority-bits", "8"],
)


def m_profile_checkpoint_test(name, firmware_name):
    """Register an M-profile checkpoint/restore test.

    Uses run_checkpoint_test.py which runs two gem5 simulations
    (save + restore) via multiprocessing.Process.
    """
    firmware_path = joinpath(
        config.base_dir,
        "tests",
        "gem5",
        "m_profile_tests",
        "programs",
        firmware_name,
    )

    if not os.path.exists(firmware_path):
        return

    verifiers = [
        verifier.MatchRegex(re.compile(r"Exiting @ tick \d+ because")),
    ]

    gem5_verify_config(
        name=f"m_profile_{name}",
        verifiers=verifiers,
        fixtures=(),
        config=joinpath(
            config.base_dir,
            "tests",
            "gem5",
            "m_profile_tests",
            "configs",
            "run_checkpoint_test.py",
        ),
        config_args=[
            "--firmware",
            firmware_path,
        ],
        valid_isas=(constants.all_compiled_tag,),
        valid_hosts=constants.supported_hosts,
        length=constants.quick_tag,
    )


m_profile_checkpoint_test("scs_checkpoint", "test_scs_checkpoint.elf")


# FreeRTOS holistic test — uses the same run_m4_test.py but with a longer
# tick limit (FreeRTOS needs many SysTick periods for context switches).
# The firmware is in the freertos/ subdirectory.
def m_profile_freertos_test():
    firmware_path = joinpath(
        config.base_dir,
        "tests",
        "gem5",
        "m_profile_tests",
        "programs",
        "freertos",
        "freertos_test.elf",
    )

    if not os.path.exists(firmware_path):
        return

    verifiers = [
        verifier.MatchRegex(re.compile(r"Exiting @ tick \d+ because")),
    ]

    gem5_verify_config(
        name="m_profile_freertos_boot",
        verifiers=verifiers,
        fixtures=(),
        config=joinpath(
            config.base_dir,
            "tests",
            "gem5",
            "m_profile_tests",
            "configs",
            "run_m4_test.py",
        ),
        config_args=[
            "--firmware",
            firmware_path,
            "--tick-limit",
            # 3 SysTick periods (TARGET_TICKS=3) ≈ 3B ticks, but 3 tasks
            # each doing vTaskDelay(1) means ~9 scheduling rounds + startup
            # + monitor polling.  10B ticks gives sufficient headroom.
            "10000000000",
        ],
        valid_isas=(constants.all_compiled_tag,),
        valid_hosts=constants.supported_hosts,
        length=constants.quick_tag,
    )


m_profile_freertos_test()


# BUG-2: FreeRTOS ICSR test — verifies ICSR dynamic fields in a real RTOS
# environment with SysTick handler, tick hook, and external IRQ pending.
def m_profile_freertos_icsr_test():
    firmware_path = joinpath(
        config.base_dir,
        "tests",
        "gem5",
        "m_profile_tests",
        "programs",
        "freertos",
        "freertos_icsr_test.elf",
    )

    if not os.path.exists(firmware_path):
        return

    verifiers = [
        verifier.MatchRegex(re.compile(r"Exiting @ tick \d+ because")),
    ]

    gem5_verify_config(
        name="m_profile_freertos_icsr",
        verifiers=verifiers,
        fixtures=(),
        config=joinpath(
            config.base_dir,
            "tests",
            "gem5",
            "m_profile_tests",
            "configs",
            "run_m4_test.py",
        ),
        config_args=[
            "--firmware",
            firmware_path,
            "--tick-limit",
            # 2 SysTick periods for tick hook to run + task scheduling.
            "10000000000",
        ],
        valid_isas=(constants.all_compiled_tag,),
        valid_hosts=constants.supported_hosts,
        length=constants.quick_tag,
    )


m_profile_freertos_icsr_test()
