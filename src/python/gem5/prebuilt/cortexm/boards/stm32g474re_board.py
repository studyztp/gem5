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
STM32G474RE timing-accurate board with Cortex-M4 pipeline,
ART accelerator caches, and bus topology matching real hardware.

Sources: RM0440 Rev 9 (STM32G4 Reference Manual),
         DDI0439D (Cortex-M4 TRM), DDI0403E (ARMv7-M ARM).

================================================================
STM32G474RE Timing Model — Expected Latency per Component
================================================================

Component              Parameter              Value    Source
------------------    --------------------   ------   ------------------
Pipeline              Stages                 3 eff    DDI0439D §2.1
Pipeline              Issue width            1        DDI0439D §2.1
Pipeline              Branch penalty         2-3 cy   DDI0439D §3.3.3

IntALU                Latency                1 cy     DDI0439D Table 3-1
IntMul                Latency                1 cy     DDI0439D Table 3-1
IntDiv                Latency                7 cy avg DDI0439D Table 3-1
FPU VADD/VMUL         Latency                1 cy     DDI0439D Table 7-1
FPU VDIV/VSQRT        Latency                14 cy    DDI0439D Table 7-1 (*)
Memory LDR            Latency                2 cy     DDI0439D Table 3-1
Memory STR            Latency                1 cy eff DDI0439D Table 3-1

ART I-Cache           Size                   1 KB     RM0440 §3.2
ART I-Cache           Lines                  32       RM0440 §3.2
ART I-Cache           Line size              32 B     RM0440 §3.2 (4x64b)
ART I-Cache           Sub-block              8 B      Flash read width
ART I-Cache           Sectors                4/line   SectorTags param
ART I-Cache           Assoc                  4-way    32 lines / 8 sets
ART I-Cache           Hit latency            1 cy     gem5 min (real=0 WS)
ART I-Cache           Prefetch               Yes      PRFTEN=1 default

ART D-Cache           Size                   256 B    RM0440 §3.2
ART D-Cache           Lines                  8        RM0440 §3.2
ART D-Cache           Line size              32 B     RM0440 §3.2 (4x64b)
ART D-Cache           Sub-block              8 B      Flash read width
ART D-Cache           Sectors                4/line   SectorTags param
ART D-Cache           Assoc                  2-way    8 lines / 4 sets
ART D-Cache           Hit latency            1 cy     gem5 min

Flash                 Wait states            4 WS     RM0440 Table 9 @170MHz
Flash                 Access latency          30 ns    5 cy x 5.88ns
SRAM                  Wait states            0 WS     RM0440 §2
SRAM                  Access latency          6 ns     1 cy x 5.88ns
CCM SRAM              Wait states            0 WS     Core-coupled
CCM SRAM              Access latency          6 ns     1 cy x 5.88ns

Bus (flash_bus)       frontend_latency       0 cy     Direct ART→flash
Bus (flash_bus)       response_latency       0 cy     No bus overhead
Bus (flash_bus)       width                  8 B      64-bit flash interface

Bus (system_bus)      frontend_latency       1 cy     AHB 1-cycle arbitration
Bus (system_bus)      response_latency       0 cy     AHB data phase in memory
Bus (system_bus)      width                  4 B      32-bit AHB

--- End-to-End Access Latency Estimates ---

Instruction fetch (ART prefetch buffer hit):
  1 cy (ART serves from buffer)

Instruction fetch (ART miss, flash read):
  0 cy (flash_bus) + 5 cy (flash) = ~5 cy

Data read from SRAM (bypasses D-cache, via system_bus):
  0 cy (dcode_bus) + 1 cy (system_bus) + 1 cy (SRAM) = ~2 cy

ART prefetch overlap (16-bit instructions):
  CPU consumes 4 insns in 4 cy; prefetch takes ~5 cy
  → ~1 cy stall per sub-block, near-zero-wait-state

(*) VDIV/VSQRT=14cy not modeled at instruction level (baseline opLat=1)
================================================================
"""

from m5.objects import (
    ArmMSystem,
    BadAddr,
    NoncoherentXBar,
    SrcClockDomain,
    VoltageDomain,
)
from m5.objects.ArmMFsWorkload import ArmMFsWorkload
from m5.objects.Cache import (
    ARTCache,
    NoncoherentCache,
)
from m5.objects.ReplacementPolicies import LRURP
from m5.objects.Tags import SectorTags

from gem5.prebuilt.cortexm.cpu.cortex_m4 import CortexM4CPU
from gem5.prebuilt.cortexm.platforms import STM32G474REPlatform


def _make_flash_bus():
    """Zero-latency crossbar modelling the direct ART-to-flash interface.

    On real STM32, the ART accelerator connects directly to the flash
    memory controller with no intermediate AHB bus matrix.  The flash
    wait states (4 WS at 170MHz = 30ns) are modeled in SimpleMemory
    latency, not here.  Width = 8 bytes (64-bit flash read interface).
    """
    bus = NoncoherentXBar(
        frontend_latency=0,
        forward_latency=0,
        response_latency=0,
        width=8,
        header_latency=0,
    )
    bus.badaddr_responder = BadAddr()
    bus.default = bus.badaddr_responder.pio
    return bus


def _make_system_bus():
    """AHB bus matrix for SRAM and peripheral access.

    Models the Cortex-M4 System bus (AHB-Lite).  32-bit data path,
    1-cycle address/arbitration phase.  The response returns without
    additional bus overhead — the memory's own latency covers the
    AHB data phase.

    Total SRAM read:  1 cy (bus arbitration) + 1 cy (SRAM 0 WS) = 2 cy.
    Total SRAM write: 1 cy (bus arbitration) + 1 cy (SRAM 0 WS) = 2 cy.
    """
    bus = NoncoherentXBar(
        frontend_latency=1,
        forward_latency=0,
        response_latency=0,
        width=4,
    )
    bus.badaddr_responder = BadAddr()
    bus.default = bus.badaddr_responder.pio
    return bus


def _make_addr_router():
    """Zero-latency NoncoherentXBar for address-based routing.

    Models the ART accelerator's address decode: flash addresses are
    routed to the ART cache, everything else goes to the default port
    (system_bus) bypassing the cache.  Zero latency because the address
    decode is part of the ART, not a physical bus.
    """
    return NoncoherentXBar(
        frontend_latency=0,
        forward_latency=0,
        response_latency=0,
        width=4,
        header_latency=0,
    )


class STM32G474RETimingBoard(ArmMSystem):
    """
    Timing-accurate STM32G474RE board matching real bus topology:
      - CortexM4CPU (single-issue 3-stage MinorCPU)
      - ART I-Cache (1KB, 4-way, sector tags, prefetch)
      - ART D-Cache (256B, 2-way, sector tags)
      - Direct ART→flash path (zero bus overhead)
      - Single AHB system_bus for SRAM/peripheral access
      - cache_line_size=8 (64-bit flash read width)

    Bus topology (matches real STM32G4 RM0440 §2.1 Figure 1):

      CPU icache_port -> stacking_barrier -> icode_bus (0-cy router)
        -> ART I-Cache (flash addrs) -> flash_bus (0-cy) -> Flash
        -> system_bus (non-flash, default) -> SRAM

      CPU dcache_port -> dcode_bus (0-cy router)
        -> ART D-Cache (flash addrs) -> flash_bus (0-cy) -> Flash
        -> system_bus (non-flash, default) -> SRAM, SCS

      MMMU stacking_port -> system_bus -> SRAM

    On real STM32, the ICode and DCode buses connect directly to the
    flash controller through the ART accelerator — they do NOT go
    through the AHB bus matrix.  Only the System bus (SRAM, peripherals)
    goes through the AHB matrix.  This topology is critical for the
    ART prefetcher to achieve near-zero-wait-state instruction fetch:
    the ~5 cy flash access overlaps with 4 cy CPU consumption of
    4 x 16-bit instructions per 8-byte sub-block.
    """

    def __init__(self, clk_freq="170MHz"):
        super().__init__()

        platform = STM32G474REPlatform()
        memories = platform.default_memories()

        self.cache_line_size = 8

        self.cpuid = platform.cpuid()

        self.voltage_domain = VoltageDomain(voltage="1.0V")
        self.clk_domain = SrcClockDomain(
            clock=clk_freq,
            voltage_domain=self.voltage_domain,
        )

        self.platform = platform

        cpu = CortexM4CPU()
        self.mem_mode = cpu.memory_mode()
        self.cpu = cpu
        self.cpu.clk_domain = self.clk_domain
        self.cpu.cpu_id = 0
        self.cpu.socket_id = 0
        self.cpu.createThreads()
        self.cpu.createInterruptController()

        # -- Buses --
        # flash_bus: zero-latency direct path from ART caches to flash.
        # system_bus: AHB bus matrix (1-cy arbitration) for SRAM + SCS.
        self.flash_bus = _make_flash_bus()
        self.system_bus = _make_system_bus()

        # -- Memories: split flash vs SRAM onto separate buses --
        flash_starts = {r.start for r in platform.code_ranges}

        self.mem_ranges = []
        for i, mem in enumerate(memories):
            setattr(self, f"mem_{i}", mem)
            self.mem_ranges.append(mem.range)
            if mem.range.start in flash_starts:
                mem.port = self.flash_bus.mem_side_ports
            else:
                mem.port = self.system_bus.mem_side_ports

        # system_bus routes flash addresses to flash_bus so that
        # gem5's system_port (functional access) can reach all memories.
        self.system_bus.mem_side_ports = self.flash_bus.cpu_side_ports
        self.system_port = self.system_bus.cpu_side_ports

        # -- SCS (NVIC + SysTick) --
        self.platform.scs.pio = self.system_bus.mem_side_ports

        # -- Address routers: zero-latency XBars for ICode/DCode decode --
        flash_ranges = platform.code_ranges

        self.icode_bus = _make_addr_router()
        self.icode_bus.default = self.system_bus.cpu_side_ports

        self.dcode_bus = _make_addr_router()
        self.dcode_bus.default = self.system_bus.cpu_side_ports

        # -- ART I-Cache: 1KB, 32 lines x 4x8B sectors, 4-way --
        # The real ART has no MSHR concept [RM0440 §3.3.4] — ideally
        # mshrs=1. However, the ART prefetch path sends requests
        # independently of MSHRs and shares the memory port, so mshrs=2
        # is needed to avoid blocking the prefetch response path.
        # TODO: Investigate ART/SectorTags interaction with mshrs=1.
        self.art_icache = ARTCache(
            size="1KiB",
            assoc=4,
            tag_latency=1,
            data_latency=1,
            response_latency=1,
            mshrs=2,
            tgts_per_mshr=2,
            write_buffers=0,
            is_read_only=True,
            sequential_access=False,
            tags=SectorTags(num_blocks_per_sector=4),
            replacement_policy=LRURP(),
            addr_ranges=flash_ranges,
            cache_blk_size=8,
            pf_blk_size=8,
            enable_prefetch=True,
            prefetch_on_cache_hit=False,
            flash_start_addr=flash_ranges[0].start,
            flash_end_addr=flash_ranges[-1].end,
        )

        # -- ART D-Cache: 256B, 8 lines x 4x8B sectors, 2-way --
        # Real ART has no MSHRs [RM0440 §3.3.4]. D-Cache has no prefetch
        # mechanism, so mshrs=1 would be ideal, but keeping mshrs=2 for
        # consistency until the blocking interaction is investigated.
        self.art_dcache = NoncoherentCache(
            size="256B",
            assoc=2,
            tag_latency=1,
            data_latency=1,
            response_latency=1,
            mshrs=2,
            tgts_per_mshr=2,
            write_buffers=0,
            is_read_only=True,
            sequential_access=False,
            tags=SectorTags(num_blocks_per_sector=4),
            replacement_policy=LRURP(),
            addr_ranges=flash_ranges,
            blk_size=8,
        )

        self._connect_cpu()

    def _connect_cpu(self):
        """Wire CPU ports with direct ART->flash and AHB->SRAM paths.

        ICode path (instruction fetch):
          CPU icache_port -> stacking_barrier -> icode_bus (0-cy router)
            -> ART I-Cache (flash addrs) -> flash_bus (0-cy) -> Flash
            -> system_bus (non-flash, default) -> SRAM

        DCode path (data access):
          CPU dcache_port -> dcode_bus (0-cy router)
            -> ART D-Cache (flash addrs) -> flash_bus (0-cy) -> Flash
            -> system_bus (non-flash, default) -> SRAM, SCS

        Stacking (exception register save/restore):
          MMMU stacking_port -> system_bus -> SRAM
        """
        self.cpu.mmu.stacking_port = self.system_bus.cpu_side_ports

        # ICode: icache_port -> stacking_barrier -> icode_bus
        self.cpu.icache_port = self.cpu.mmu.stacking_barrier.cpu_side_port
        self.cpu.mmu.stacking_barrier.mem_side_port = (
            self.icode_bus.cpu_side_ports
        )
        self.art_icache.cpu_side = self.icode_bus.mem_side_ports
        self.art_icache.mem_side = self.flash_bus.cpu_side_ports

        # DCode: dcache_port -> dcode_bus
        self.cpu.dcache_port = self.dcode_bus.cpu_side_ports
        self.art_dcache.cpu_side = self.dcode_bus.mem_side_ports
        self.art_dcache.mem_side = self.flash_bus.cpu_side_ports

    def set_workload(self, firmware_path):
        """Set the bare-metal firmware ELF to execute."""
        self.workload = ArmMFsWorkload(object_file=firmware_path)
