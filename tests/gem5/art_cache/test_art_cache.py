"""
Hardware-fidelity tests for the ART (Adaptive Real-Time) accelerator cache.

Each test scenario validates a specific behaviour documented in the STM32
ART accelerator specification to ensure the gem5 model faithfully
reproduces the real hardware:

  1. hw_sequential_prefetch   -- PM0059 §2.4.2 sequential prefetch pipeline
  2. hw_branch_penalty        -- non-sequential access defeats prefetch
  3. hw_buffer_promotion      -- two-buffer promotion mechanism
  4. hw_prefetch_disable      -- PRFTEN=0 disables all prefetch activity
  5. hw_direct_memory_bypass  -- direct flash access path (no ART)
  6. hw_flash_range_bounds    -- prefetch limited to flash address region
  7. hw_completion_fidelity   -- prefetch lifecycle state machine check

Tests that use the prefetch mechanism with the I-Cache are registered in
two variants: one with prefetch_on_cache_hit=False (conservative, default)
and one with prefetch_on_cache_hit=True (eager), since the STM32
documentation does not specify the exact hardware behaviour.
"""

from testlib import *

prefetch_tests = [
    "hw_sequential_prefetch",
    "hw_branch_penalty",
    "hw_buffer_promotion",
    "hw_flash_range_bounds",
    "hw_completion_fidelity",
]

no_prefetch_tests = [
    "hw_prefetch_disable",
    "hw_direct_memory_bypass",
]

for test_type in prefetch_tests:
    gem5_verify_config(
        name=f"art_cache_{test_type}_no_pf_on_hit",
        verifiers=(),
        config=joinpath(getcwd(), "configs", "art_test_run.py"),
        config_args=["--test-type", test_type],
        valid_isas=(constants.null_tag,),
        length=constants.long_tag,
    )
    gem5_verify_config(
        name=f"art_cache_{test_type}_pf_on_hit",
        verifiers=(),
        config=joinpath(getcwd(), "configs", "art_test_run.py"),
        config_args=["--test-type", test_type, "--prefetch-on-cache-hit"],
        valid_isas=(constants.null_tag,),
        length=constants.long_tag,
    )

for test_type in no_prefetch_tests:
    gem5_verify_config(
        name=f"art_cache_{test_type}",
        verifiers=(),
        config=joinpath(getcwd(), "configs", "art_test_run.py"),
        config_args=["--test-type", test_type],
        valid_isas=(constants.null_tag,),
        length=constants.long_tag,
    )
