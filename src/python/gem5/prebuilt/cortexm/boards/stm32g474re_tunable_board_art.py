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

Tunable parameters (defaults updated 2026-05-10 to mirror the no-ART
sweep winner — `art_address_phase_latency` and `art_prefetch_buffer_hit_latency`
were copied verbatim from the PipelinedSimpleMemory calibration; the
ART model's parameters mean different things (line-cache hit, not
sense-amp latch hit), so this calibration is a starting point and
should be re-validated against the ART HW reference):

    art_flash_latency           = "23000ps"  (SimpleMemory.latency,
                                              raw flash array access)
    art_address_phase_latency   = "600ps"    (ART cache addr phase)
    art_prefetch_buffer_hit_latency      = "5000ps"   (ART cache hit latency)
    art_enable_cache            = True       (False → bypass cache)
    art_enable_prefetch         = True       (False → no prefetcher)

The cache and prefetch toggles are independent, so all four
combinations are reachable: full ART (default), cache-only,
prefetch-only, neither.  When ``art_enable_cache=False`` the
underlying NoncoherentCache is bypassed and demand misses go
straight to flash, but the per-bank prefetch buffers still serve
hits unless ``art_enable_prefetch=False`` is also set.
"""

from m5.objects import SimpleMemory

from gem5.prebuilt.cortexm.boards.stm32g474re_board import (
    STM32G474RETimingBoard,
)


class STM32G474RETunableBoardART(STM32G474RETimingBoard):
    """STM32G474RE board with runtime-tunable ART cache parameters.

    Always uses ``enable_art=True``.  Exposes the underlying flash
    SimpleMemory latency and the ART cache's address_phase_latency
    and prefetch_buffer_hit_latency for parameter sweeps.

    Parameters
    ----------
    art_flash_latency : str, default ``"23000ps"``
        ``SimpleMemory.latency`` for the flash banks behind the ART
        cache (raw flash array access time).

    art_address_phase_latency : str, default ``"600ps"``
        ``ARTCache.address_phase_latency`` (AHB address phase the
        ART cache adds on its outbound flash read).

    art_prefetch_buffer_hit_latency : str, default ``"5000ps"``
        ``ARTCache.prefetch_buffer_hit_latency`` (latency on a sense-amp
        buffer hit inside the ART cache).

    art_enable_cache : bool, default ``True``
        When ``False``, sets ``ARTCache.direct_memory_mode=True``:
        demand misses bypass the underlying cache and go straight to
        flash; cache fills are skipped.  The per-bank prefetch
        buffers still serve hits, so prefetching remains effective
        unless ``art_enable_prefetch=False`` is also passed.

    art_enable_prefetch : bool, default ``True``
        When ``False``, sets ``ARTCache.enable_prefetch=False``:
        the sequential instruction prefetcher is disabled.  The
        underlying cache still fills/hits unless
        ``art_enable_cache=False`` is also passed.
    """

    def __init__(
        self,
        # Matched with the no-ART board's flash_latency (29000ps).  The
        # underlying silicon flash takes the same time regardless of
        # whether ART is enabled — 4 wait states + 1 access cycle =
        # 5 HCLK at 170 MHz ≈ 29 ns (RM0440 Table 19).  Prior value
        # of 23000ps was a calibration hack that compensated for
        # ART's structural serialization overhead; with the ART
        # refactor in progress we want to model the actual flash
        # access time and let the ART pipeline absorb the overlap.
        art_flash_latency: str = "29000ps",
        art_address_phase_latency: str = "600ps",
        art_prefetch_buffer_hit_latency: str = "5000ps",
        art_enable_pipeline: bool = True,
        art_arrive_buffer_size: int = 1,
        art_port_ahb_buffer_size: int = 8,
        art_port_ahb_buffer_latency: str = "0ns",
        art_enable_cache: bool = True,
        art_enable_prefetch: bool = True,
        art_max_outstanding_requests: int = 2,
        art_psm_compatible_bypass: bool = True,
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

        # Patch flash SimpleMemory latency.  Note:
        # include_receive_delay is *unconditionally* False on the
        # ART flash backing — ART has a direct flash interface, no
        # bus between, so receive_delay is always wrong here.  Set
        # in platforms.py at SimpleMemory construction; not exposed
        # as a runtime knob (no useful "true" alternative).
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
            if hasattr(cache, "prefetch_buffer_hit_latency"):
                cache.prefetch_buffer_hit_latency = (
                    art_prefetch_buffer_hit_latency
                )
            if hasattr(cache, "enable_pipeline"):
                cache.enable_pipeline = art_enable_pipeline
            if hasattr(cache, "arrive_buffer_size"):
                cache.arrive_buffer_size = art_arrive_buffer_size
            if hasattr(cache, "port_ahb_buffer_size"):
                cache.port_ahb_buffer_size = art_port_ahb_buffer_size
            if hasattr(cache, "port_ahb_buffer_latency"):
                cache.port_ahb_buffer_latency = art_port_ahb_buffer_latency
            # ART cache + prefetch toggles.  ARTCache uses positive
            # `enable_prefetch` and *negative* `direct_memory_mode`
            # (True = bypass cache).  Convert the positive
            # `art_enable_cache` knob to the negative ARTCache field
            # here so the user-facing API stays uniformly positive.
            if hasattr(cache, "direct_memory_mode"):
                cache.direct_memory_mode = not art_enable_cache
            # NOTE: only the I-cache has a hardware prefetcher per
            # RM0440 §3.3.4 — the D-cache has no prefetch buffer.
            # The board pins art_dcache.enable_prefetch=False at
            # instantiation; do not let the runtime knob override
            # it (would model a non-physical D-side prefetcher).
            if hasattr(cache, "enable_prefetch") and name != "art_dcache":
                cache.enable_prefetch = art_enable_prefetch
            # Stage 0 (issue 2026-05-10-art-bypass-vs-no-art-divergence):
            # AHB-Lite back-to-back pipeline depth, and PSM-compatible
            # bypass routing.  Both wired in but inert until Stage 3+.
            if hasattr(cache, "max_outstanding_requests"):
                cache.max_outstanding_requests = art_max_outstanding_requests
            if hasattr(cache, "psm_compatible_bypass"):
                cache.psm_compatible_bypass = art_psm_compatible_bypass
            patched_cache.append(name)

        self._tunable_art_flash_latency = art_flash_latency
        self._tunable_art_address_phase_latency = art_address_phase_latency
        self._tunable_art_prefetch_buffer_hit_latency = (
            art_prefetch_buffer_hit_latency
        )
        self._tunable_art_enable_pipeline = art_enable_pipeline
        self._tunable_art_arrive_buffer_size = art_arrive_buffer_size
        self._tunable_art_port_ahb_buffer_size = art_port_ahb_buffer_size
        self._tunable_art_port_ahb_buffer_latency = art_port_ahb_buffer_latency
        self._tunable_art_enable_cache = art_enable_cache
        self._tunable_art_enable_prefetch = art_enable_prefetch
        self._tunable_art_max_outstanding_requests = (
            art_max_outstanding_requests
        )
        self._tunable_art_psm_compatible_bypass = art_psm_compatible_bypass
        self._tunable_art_patched_mem = tuple(patched_mem)
        self._tunable_art_patched_cache = tuple(patched_cache)

    def tunable_art_summary(self) -> str:
        return (
            f"art[flash_lat={self._tunable_art_flash_latency}, "
            f"addr_phase={self._tunable_art_address_phase_latency}, "
            f"buf_hit={self._tunable_art_prefetch_buffer_hit_latency}, "
            f"pipeline={self._tunable_art_enable_pipeline}, "
            f"arr_buf={self._tunable_art_arrive_buffer_size}, "
            f"ahb_buf_sz={self._tunable_art_port_ahb_buffer_size}, "
            f"ahb_buf_lat={self._tunable_art_port_ahb_buffer_latency}, "
            f"cache={self._tunable_art_enable_cache}, "
            f"prefetch={self._tunable_art_enable_prefetch}, "
            f"max_outstanding={self._tunable_art_max_outstanding_requests}, "
            f"psm_compat={self._tunable_art_psm_compatible_bypass}, "
            f"mem={list(self._tunable_art_patched_mem)}, "
            f"caches={list(self._tunable_art_patched_cache)}]"
        )
