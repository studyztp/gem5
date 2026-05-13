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
STM32G474RE TunableBoard — exposes Flash memory timing knobs for
calibration sweeps.

Subclasses STM32G474RETimingBoard and rewrites every
PipelinedSimpleMemory child's timing parameters in-place after the
parent constructor runs.  Enables runscript-driven sweeps without
requiring a gem5 rebuild for each parameter trial.

Default values, recalibrated 2026-05-13 against the gem5-vs-board-microbench-cycles
nop16 sweep (slope target: 1.5 cy/NOP for `none` config — see
issues/2026-05-13-nop16-none-front-end-rate/):

    flash_latency               = "29411ps"   (5 HCLK = 4 wait states + 1 access cycle, RM0440 §3.3.3)
    flash_address_phase_latency = "600ps"
    flash_buffer_hit_latency    = "5882ps"    (1 HCLK = AHB address phase after sense-amp hit)
    flash_read_buffer_size      = 8           (64-bit sense-amp latch)

The new sum (flash_latency + flash_buffer_hit_latency = 35293ps) equals
6 HCLK at 170 MHz — the empirically-measured silicon period for one
sequential 64-bit line in the no-ART path. This matches HW slope of
1.5 cy/NOP exactly across the full nop16_{8,16,…,80,100} sweep.

Prior calibration (29000ps + 5000ps = 34000ps) was tuned for
bench-alu / bench-alu16 / bench-branch, which don't exercise the
flash steady-state rate the way nop16 does — they were within ~1.14 %
of silicon. The 1294ps shortfall (≈ 0.22 HCLK) only became visible
once the nop16 scaling sweep landed.

Usage from a runscript:

    from gem5.prebuilt.cortexm.boards.stm32g474re_tunable_board import (
        STM32G474RETunableBoard,
    )

    board = STM32G474RETunableBoard(
        cpu_cls=ArmMSignalCPU,
        flash_latency="29000ps",
        flash_address_phase_latency="400ps",
        flash_read_buffer_size=8,
    )
"""

from m5.objects import PipelinedSimpleMemory

from gem5.prebuilt.cortexm.boards.stm32g474re_board import (
    STM32G474RETimingBoard,
)


class STM32G474RETunableBoard(STM32G474RETimingBoard):
    """STM32G474RE board with runtime-tunable Flash timing parameters.

    Always uses ``enable_art=False`` (the calibrated configuration);
    the ART path uses a different memory model and isn't tunable
    through this hook.  All other ``STM32G474RETimingBoard`` arguments
    pass through unchanged via ``**kwargs``.

    Parameters
    ----------
    flash_latency : str, default ``"29411ps"``
        ``PipelinedSimpleMemory.latency`` (Flash data-phase access
        time). 5 HCLK at 170 MHz = 4 wait states + 1 access cycle
        (RM0440 §3.3.3 Table 17).

    flash_address_phase_latency : str, default ``"600ps"``
        ``PipelinedSimpleMemory.address_phase_latency`` (AHB address
        phase, models the ICode bus address-phase setup).

    flash_buffer_hit_latency : str, default ``"5882ps"``
        ``PipelinedSimpleMemory.buffer_hit_latency`` (AHB address-phase
        cycle after a Flash sense-amp hit; 1 HCLK at 170 MHz).
        Combined with flash_latency this yields 6 HCLK total per
        64-bit line, matching silicon's no-ART steady-state rate.

    flash_read_buffer_size : int, default ``8``
        ``PipelinedSimpleMemory.port_read_buffer_size`` per port
        (64-bit Flash sense-amp output latch, RM0440 §4.3.4).
        Set to 0 to disable the buffer entirely.
    """

    def __init__(
        self,
        flash_latency: str = "29411ps",
        flash_address_phase_latency: str = "600ps",
        flash_buffer_hit_latency: str = "5882ps",
        flash_read_buffer_size: int = 8,
        **kwargs,
    ):
        # The tunable board only makes sense with ART disabled —
        # the parent's ART path uses SimpleMemory (not
        # PipelinedSimpleMemory), and ART caching dominates the
        # CPU-visible Flash latency anyway.
        if kwargs.get("enable_art", False):
            raise ValueError(
                "STM32G474RETunableBoard requires enable_art=False; "
                "Flash timing knobs are only meaningful in the "
                "no-ART path."
            )
        kwargs["enable_art"] = False

        super().__init__(**kwargs)

        # Patch every PipelinedSimpleMemory child's timing in place.
        # gem5 SimObject children live in `_children` (dir() doesn't
        # see them), so we walk the dict directly.
        patched = []
        for name, child in self._children.items():
            if isinstance(child, PipelinedSimpleMemory):
                child.latency = flash_latency
                child.address_phase_latency = flash_address_phase_latency
                child.buffer_hit_latency = flash_buffer_hit_latency
                child.port_read_buffer_size = [
                    flash_read_buffer_size,
                    flash_read_buffer_size,
                ]
                patched.append(name)

        # Stash the applied params + the patched-child names on the
        # board so runscripts can introspect or assert on them.
        self._tunable_flash_latency = flash_latency
        self._tunable_flash_address_phase_latency = flash_address_phase_latency
        self._tunable_flash_buffer_hit_latency = flash_buffer_hit_latency
        self._tunable_flash_read_buffer_size = flash_read_buffer_size
        self._tunable_flash_patched_children = tuple(patched)

    def tunable_flash_summary(self) -> str:
        """One-line summary of the active Flash params (for traces /
        printouts)."""
        return (
            f"flash[lat={self._tunable_flash_latency}, "
            f"addr_phase={self._tunable_flash_address_phase_latency}, "
            f"buf_hit={self._tunable_flash_buffer_hit_latency}, "
            f"rbuf={self._tunable_flash_read_buffer_size}, "
            f"patched={list(self._tunable_flash_patched_children)}]"
        )
