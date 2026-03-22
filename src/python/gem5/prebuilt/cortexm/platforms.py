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
Pre-configured ArmMPlatform subclasses for specific STM32 variants.

Each class sets the memory ranges and SCS configuration to match a
real Cortex-M chip's datasheet.  All values come from the official
STMicroelectronics reference manuals and datasheets cited in each
class's docstring.

== How to add a new platform ==

1. Find your chip's reference manual (search for "RM" number on st.com).
2. Look up the memory map table (usually in chapter 2).
3. Note the flash base/size, each SRAM block's base/size, and whether
   a boot alias exists at 0x00000000.
4. Look up the NVIC section for the number of external interrupts.
5. Look up the priority bits (usually 4 on Cortex-M4, 2 on Cortex-M0).
6. Subclass ArmMPlatform and set the parameters:

    from m5.objects.MProfilePlatform import ArmMPlatform
    from m5.objects.MProfileSCS import MProfileSCS
    from m5.params import AddrRange

    class MyChipPlatform(ArmMPlatform):
        \"""MyChip — Cortex-M4, 256KB Flash, 64KB SRAM.\"""
        code_ranges = [AddrRange(0x08000000, size='256KiB')]
        sram_ranges = [AddrRange(0x20000000, size='64KiB')]
        scs = MProfileSCS(num_irqs=32, priority_bits=4)

That's it — no C++ needed.  The ArmMBoard (or your own config script)
will create SimpleMemory objects for each range and wire them to the bus.
"""

from m5.objects.MProfilePlatform import ArmMPlatform
from m5.objects.MProfileSCS import MProfileSCS
from m5.objects.SimpleMemory import SimpleMemory
from m5.params import AddrRange


class STM32F405Platform(ArmMPlatform):
    """
    STM32F405RG — Cortex-M4 @ 168MHz.

    Reference: STM32F405xx datasheet (DS8597 Rev 9),
               RM0090 reference manual (chapter 2, memory map).

    Memory layout:
      Flash:  1MB     @ 0x08000000  (single bank)
      SRAM1:  112KB   @ 0x20000000  (main SRAM, cached, DMA-capable)
      SRAM2:  16KB    @ 0x2001C000  (non-cached, DMA-capable)
      CCM:    64KB    @ 0x10000000  (core-coupled, NOT DMA-accessible)

    Boot alias: flash is mirrored to 0x00000000 (configurable via
    BOOT pins; this is the default boot-from-flash configuration).

    NVIC: 82 external IRQs, 4-bit priority (16 levels).
    """

    # -- Flash --
    # Single bank, 1MB.  On STM32F405, flash is always at 0x08000000.
    code_ranges = [AddrRange(0x08000000, size="1MiB")]

    # -- SRAM --
    # Three separate blocks at non-contiguous addresses:
    #
    # SRAM1 (112KB @ 0x20000000):
    #   Main SRAM.  Cached via ART Accelerator.  Accessible by
    #   both CPU and DMA.  Stack and heap typically live here.
    #
    # SRAM2 (16KB @ 0x2001C000):
    #   Contiguous after SRAM1 in address space.  Not cached.
    #   DMA-accessible.  Often used for DMA buffers.
    #
    # CCM SRAM (64KB @ 0x10000000):
    #   Core-Coupled Memory — connected directly to the D-bus.
    #   NOT accessible by DMA controllers (hardware limitation).
    #   Useful for time-critical data (interrupt vectors, stacks)
    #   because it has zero-wait-state access.
    #   Note: address 0x10000000 is OUTSIDE the normal SRAM region
    #   (0x20000000-0x3FFFFFFF) — it sits in the Code region.
    sram_ranges = [
        AddrRange(0x20000000, size="112KiB"),  # SRAM1
        AddrRange(0x2001C000, size="16KiB"),  # SRAM2
        AddrRange(0x10000000, size="64KiB"),  # CCM (no DMA)
    ]

    # -- Peripherals --
    # APB1 + APB2 + AHB1 + AHB2 all live in 0x40000000-0x5FFFFFFF.
    # We model this as one range; individual peripherals can be
    # added via the future pluggable device framework.
    periph_ranges = [AddrRange(0x40000000, size="512MiB")]

    # -- Boot alias --
    # Flash is aliased to 0x00000000 when booting from flash
    # (BOOT0=0 or BOOT0=1/BOOT1=0).  The vector table at address 0
    # is the first thing the CPU reads on reset (initial SP + PC).
    boot_alias_ranges = [AddrRange(0x00000000, size="1MiB")]

    # -- SCS --
    # 82 external IRQs (RM0090 Table 62: position 0-81).
    # 4-bit priority (16 programmable levels, standard for Cortex-M4).
    # SysTick and BASEPRI/FAULTMASK are present (Cortex-M4).
    scs = MProfileSCS(
        num_irqs=82,
        priority_bits=4,
        has_systick=True,
        has_basepri=True,
    )

    def cpuid(self):
        """CPUID for Cortex-M4 r0p1 (DDI0403E B3.2.3)."""
        return 0x410FC241

    def default_memories(self):
        """
        Create default SimpleMemory objects matching this chip's layout.

        Returns a list of memories with reasonable latencies for a
        Cortex-M4 @ 168MHz.  Override or create your own list for
        more accurate modelling (e.g., flash with ART accelerator).

        Note: No boot alias memory at 0x0 is created.  A correctly
        built STM32 firmware ELF has its vector table and code at
        0x08000000 (the real flash address).  The linker script
        sets VTOR to 0x08000000, so the CPU reads initial SP/PC
        from there — no mirror at 0x0 needed.

        Usage:
            platform = STM32F405Platform()
            board = ArmMBoard(
                platform=platform,
                cpu=ArmMAtomicSimpleCPU(),
                memories=platform.default_memories(),
            )
        """
        return [
            # Flash: ~30ns models typical 5-6 wait states at 168MHz.
            # In real hardware, the ART accelerator hides most of
            # this latency for sequential instruction fetches.
            SimpleMemory(
                range=AddrRange(0x08000000, size="1MiB"),
                latency="30ns",
            ),
            # SRAM1: zero wait state at 168MHz.
            SimpleMemory(
                range=AddrRange(0x20000000, size="112KiB"),
                latency="5ns",
            ),
            # SRAM2: zero wait state, non-cached.
            SimpleMemory(
                range=AddrRange(0x2001C000, size="16KiB"),
                latency="5ns",
            ),
            # CCM SRAM: zero wait state, no DMA access.
            SimpleMemory(
                range=AddrRange(0x10000000, size="64KiB"),
                latency="5ns",
            ),
        ]


class STM32G474REPlatform(ArmMPlatform):
    """
    STM32G474RE — Cortex-M4 @ 170MHz.

    Reference: STM32G474xE datasheet (DS12288 Rev 5),
               RM0440 reference manual (chapter 2, memory map).

    Memory layout:
      Flash Bank 1:  256KB  @ 0x08000000
      Flash Bank 2:  256KB  @ 0x08040000
      SRAM1:         80KB   @ 0x20000000
      SRAM2:         16KB   @ 0x20014000
      CCM SRAM:      32KB   @ 0x10000000  (also aliased at 0x20018000)

    Dual-bank flash supports read-while-write: the CPU can execute
    from one bank while the other bank is being erased/programmed.

    Boot alias: Bank 1 is mirrored to 0x00000000 by default.

    NVIC: 102 external IRQs, 4-bit priority (16 levels).
    """

    # -- Flash --
    # Dual-bank: two 256KB banks at separate addresses.
    # Bank swapping is configurable via FB_MODE in SYSCFG.
    code_ranges = [
        AddrRange(0x08000000, size="256KiB"),  # Flash Bank 1
        AddrRange(0x08040000, size="256KiB"),  # Flash Bank 2
    ]

    # -- SRAM --
    # Three blocks plus an alias:
    #
    # SRAM1 (80KB @ 0x20000000):
    #   Main SRAM.  DMA-accessible.
    #
    # SRAM2 (16KB @ 0x20014000):
    #   Contiguous after SRAM1.  Supports hardware parity check
    #   and per-page write protection (1KB granularity).
    #
    # CCM SRAM (32KB @ 0x10000000):
    #   Core-coupled memory with hardware parity check.
    #   Also aliased at 0x20018000 (immediately after SRAM2) to
    #   provide a contiguous 128KB view from 0x20000000-0x2001FFFF.
    #   We model only the primary address; the alias can be added
    #   as a future enhancement.
    sram_ranges = [
        AddrRange(0x20000000, size="80KiB"),  # SRAM1
        AddrRange(0x20014000, size="16KiB"),  # SRAM2
        AddrRange(0x10000000, size="32KiB"),  # CCM SRAM
    ]

    # -- Peripherals --
    periph_ranges = [AddrRange(0x40000000, size="512MiB")]

    # -- Boot alias --
    # Flash Bank 1 mirrored to 0x00000000.
    boot_alias_ranges = [AddrRange(0x00000000, size="256KiB")]

    # -- SCS --
    # 102 external IRQs (RM0440 Table 98: position 0-101).
    # 4-bit priority (16 programmable levels).
    scs = MProfileSCS(
        num_irqs=102,
        priority_bits=4,
        has_systick=True,
        has_basepri=True,
    )

    def cpuid(self):
        """CPUID for Cortex-M4 r0p1 (DDI0403E B3.2.3)."""
        return 0x410FC241

    def default_memories(self):
        """
        Create default SimpleMemory objects matching this chip's layout.

        Returns a list of memories with reasonable latencies for a
        Cortex-M4 @ 170MHz.  No boot alias — correctly linked
        firmware places the vector table at 0x08000000.
        """
        return [
            # Flash Bank 1: ~30ns models wait states at 170MHz.
            SimpleMemory(
                range=AddrRange(0x08000000, size="256KiB"),
                latency="30ns",
            ),
            # Flash Bank 2: same latency as Bank 1.
            SimpleMemory(
                range=AddrRange(0x08040000, size="256KiB"),
                latency="30ns",
            ),
            # SRAM1: zero wait state.
            SimpleMemory(
                range=AddrRange(0x20000000, size="80KiB"),
                latency="5ns",
            ),
            # SRAM2: zero wait state, hardware parity check.
            SimpleMemory(
                range=AddrRange(0x20014000, size="16KiB"),
                latency="5ns",
            ),
            # CCM SRAM: zero wait state, hardware parity check.
            SimpleMemory(
                range=AddrRange(0x10000000, size="32KiB"),
                latency="5ns",
            ),
        ]
