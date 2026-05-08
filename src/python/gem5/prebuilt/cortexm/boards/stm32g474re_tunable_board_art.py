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
STM32G474RE TunableBoardART — exposes ART cache and Flash timing knobs
for calibration sweeps with ART enabled.

Subclasses STM32G474RETimingBoard with enable_art=True, then rewrites
the SimpleMemory backing flash latency and the ART cache parameters
in-place after the parent constructor runs.

Tunable parameters (with defaults from stm32g474re_board.py):

    art_flash_latency           = "23000ps"  (SimpleMemory.latency,
                                              raw flash array access)
    art_address_phase_latency   = "500ps"    (ART cache addr phase)
    art_buffer_hit_latency      = "0ns"      (ART cache hit latency,
                                              real HW = 0 WS)
"""

from m5.objects import SimpleMemory

from gem5.prebuilt.cortexm.boards.stm32g474re_board import (
    STM32G474RETimingBoard,
)


class STM32G474RETunableBoardART(STM32G474RETimingBoard):
    """STM32G474RE board with runtime-tunable ART cache parameters.

    Always uses ``enable_art=True``.  Exposes the underlying flash
    SimpleMemory latency and the ART cache's address_phase_latency
    and buffer_hit_latency for parameter sweeps.

    Parameters
    ----------
    art_flash_latency : str, default ``"23000ps"``
        ``SimpleMemory.latency`` for the flash banks behind the ART
        cache (raw flash array access time).

    art_address_phase_latency : str, default ``"500ps"``
        ``ARTCache.address_phase_latency`` (AHB address phase the
        ART cache adds on its outbound flash read).

    art_buffer_hit_latency : str, default ``"0ns"``
        ``ARTCache.buffer_hit_latency`` (latency on a sense-amp
        buffer hit inside the ART cache).
    """

    def __init__(
        self,
        art_flash_latency: str = "23000ps",
        art_address_phase_latency: str = "500ps",
        art_buffer_hit_latency: str = "0ns",
        art_enable_pipeline: bool = True,
        art_arrive_buffer_size: int = 1,
        art_port_ahb_buffer_size: int = 8,
        art_port_ahb_buffer_latency: str = "0ns",
        **kwargs,
    ):
        # Force ART on; that is the whole point of this tunable.
        if "enable_art" in kwargs and not kwargs["enable_art"]:
            raise ValueError(
                "STM32G474RETunableBoardART requires enable_art=True; "
                "use STM32G474RETunableBoard for the no-ART path."
            )
        kwargs["enable_art"] = True

        super().__init__(**kwargs)

        # Patch flash SimpleMemory latency.
        patched_mem = []
        for name, child in self._children.items():
            if isinstance(child, SimpleMemory):
                start = int(child.range.start)
                if 0x08000000 <= start < 0x08080000:
                    child.latency = art_flash_latency
                    patched_mem.append(name)

        # Patch ART caches: art_icache + art_dcache.
        patched_cache = []
        for name in ("art_icache", "art_dcache"):
            cache = getattr(self, name, None)
            if cache is None:
                continue
            if hasattr(cache, "address_phase_latency"):
                cache.address_phase_latency = art_address_phase_latency
            if hasattr(cache, "buffer_hit_latency"):
                cache.buffer_hit_latency = art_buffer_hit_latency
            if hasattr(cache, "enable_pipeline"):
                cache.enable_pipeline = art_enable_pipeline
            if hasattr(cache, "arrive_buffer_size"):
                cache.arrive_buffer_size = art_arrive_buffer_size
            if hasattr(cache, "port_ahb_buffer_size"):
                cache.port_ahb_buffer_size = art_port_ahb_buffer_size
            if hasattr(cache, "port_ahb_buffer_latency"):
                cache.port_ahb_buffer_latency = art_port_ahb_buffer_latency
            patched_cache.append(name)

        self._tunable_art_flash_latency = art_flash_latency
        self._tunable_art_address_phase_latency = art_address_phase_latency
        self._tunable_art_buffer_hit_latency = art_buffer_hit_latency
        self._tunable_art_enable_pipeline = art_enable_pipeline
        self._tunable_art_arrive_buffer_size = art_arrive_buffer_size
        self._tunable_art_port_ahb_buffer_size = art_port_ahb_buffer_size
        self._tunable_art_port_ahb_buffer_latency = art_port_ahb_buffer_latency
        self._tunable_art_patched_mem = tuple(patched_mem)
        self._tunable_art_patched_cache = tuple(patched_cache)

    def tunable_art_summary(self) -> str:
        return (
            f"art[flash_lat={self._tunable_art_flash_latency}, "
            f"addr_phase={self._tunable_art_address_phase_latency}, "
            f"buf_hit={self._tunable_art_buffer_hit_latency}, "
            f"pipeline={self._tunable_art_enable_pipeline}, "
            f"arr_buf={self._tunable_art_arrive_buffer_size}, "
            f"ahb_buf_sz={self._tunable_art_port_ahb_buffer_size}, "
            f"ahb_buf_lat={self._tunable_art_port_ahb_buffer_latency}, "
            f"mem={list(self._tunable_art_patched_mem)}, "
            f"caches={list(self._tunable_art_patched_cache)}]"
        )
