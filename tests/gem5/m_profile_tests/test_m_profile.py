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


def m_profile_test(name, firmware_name, expected_result="pass"):
    """Register an M-profile integration test.

    Args:
        name: Test name (e.g., "test_cps")
        firmware_name: ELF filename (e.g., "test_cps.elf")
        expected_result: "pass" (default) or "fail" — what the firmware
            is expected to report.  Controls --expected-result flag
            passed to run_m4_test.py.
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
        config_args=[
            "--firmware",
            firmware_path,
            "--expected-result",
            expected_result,
        ],
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
