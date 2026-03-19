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

from m5.objects.MProfileSCS import MProfileSCS
from m5.objects.Platform import Platform
from m5.params import *


class ArmMPlatform(Platform):
    """
    Fully configurable M-profile (Cortex-M) platform.

    == Why regions are lists, not single ranges ==

    The ARMv7-M architecture defines 8 high-level memory regions
    (Code, SRAM, Peripheral, External RAM, External Device, PPB,
    Vendor-specific, and System).  However, real Cortex-M vendors
    split these into multiple non-contiguous physical blocks.

    For example, STM32F405 has three separate SRAM blocks:
      - SRAM1: 112KB @ 0x20000000
      - SRAM2:  16KB @ 0x2001C000
      - CCM:    64KB @ 0x10000000  (not even in the SRAM region!)

    And STM32G474RE has two flash banks at different addresses:
      - Bank 1: 256KB @ 0x08000000
      - Bank 2: 256KB @ 0x08040000

    A single AddrRange cannot express these.  Therefore, every
    region parameter is a VectorParam.AddrRange (a list of ranges).
    Each entry in the list becomes a separate SimpleMemory object
    connected to the system bus.

    The ONLY architecturally fixed address is the SCS (System Control
    Space) at 0xE000E000, which is inside the Private Peripheral Bus
    region.  Everything else is vendor-defined.

    == How to create a custom platform ==

    Subclass ArmMPlatform and override the range parameters:

        class MyChipPlatform(ArmMPlatform):
            code_ranges = [AddrRange(0x08000000, size='512KiB')]
            sram_ranges = [
                AddrRange(0x20000000, size='64KiB'),  # SRAM1
                AddrRange(0x10000000, size='16KiB'),  # CCM
            ]
            scs = MProfileSCS(num_irqs=42, priority_bits=3)

    See STM32F405Platform and STM32G474REPlatform (in the gem5
    stdlib) for complete real-world examples.
    """

    type = "ArmMPlatform"
    cxx_class = "gem5::ArmMPlatform"
    cxx_header = "dev/arm/m_profile_platform.hh"

    # ==================================================================
    # Memory region parameters
    # ==================================================================
    #
    # Each parameter is a list of AddrRange because real Cortex-M
    # chips can have multiple banks/blocks per region type.
    # Empty list ([]) means the region is not present on this variant.

    # -- Code (Flash) --
    # Firmware executes from here.  The vector table (VTOR) typically
    # points into this region.  Multiple banks are common on higher-end
    # chips (e.g., dual-bank flash for read-while-write).
    # STM32 convention: 0x08000000.  NXP/Nordic: 0x00000000.
    code_ranges = VectorParam.AddrRange(
        [AddrRange(0x08000000, size="256KiB")],
        "Flash/code memory regions.  Each entry is a separate bank. "
        "Multiple entries for dual-bank flash (e.g., STM32G4).",
    )

    # -- SRAM --
    # On-chip volatile memory for data, stack, heap.  Almost always
    # starts at 0x20000000, but chips often have multiple blocks
    # (SRAM1, SRAM2, CCM) at different addresses and sizes.
    # CCM SRAM may be at 0x10000000 (outside the 0x20000000 region).
    sram_ranges = VectorParam.AddrRange(
        [AddrRange(0x20000000, size="64KiB")],
        "SRAM regions.  Each entry is a separate block "
        "(e.g., SRAM1, SRAM2, CCM SRAM at different addresses).",
    )

    # -- Peripherals --
    # On-chip peripheral bus (UART, SPI, I2C, GPIO, timers, etc.).
    # ARMv7-M reserves 0x40000000-0x5FFFFFFF for this.
    # Typically one large range, but some vendors subdivide into
    # APB1, APB2, AHB1, AHB2 with gaps between them.
    periph_ranges = VectorParam.AddrRange(
        [AddrRange(0x40000000, size="512MiB")],
        "On-chip peripheral bus regions (APB1, APB2, AHB, etc.).",
    )

    # -- External RAM (optional) --
    # Off-chip SDRAM, PSRAM, or QSPI-mapped memory via FMC/FSMC.
    # ARMv7-M reserves 0x60000000-0x9FFFFFFF for this.
    # Empty list if this chip has no external memory interface.
    external_ram_ranges = VectorParam.AddrRange(
        [],
        "External RAM regions (FMC/QSPI).  Empty if not present.",
    )

    # -- External Device (optional) --
    # Off-chip memory-mapped devices (LCD controllers, etc.).
    # ARMv7-M reserves 0xA0000000-0xDFFFFFFF for this.
    external_device_ranges = VectorParam.AddrRange(
        [],
        "External device regions.  Empty if not present.",
    )

    # -- Private Peripheral Bus --
    # Contains the SCS (NVIC+SysTick+SCB) at the architecturally
    # fixed address 0xE000E000, plus optional debug components
    # (ITM, DWT, FPB, TPIU) in the rest of the 1MB region.
    # This is NOT a VectorParam because the base address is fixed
    # by the ARM architecture — it is never vendor-configurable.
    ppb_range = Param.AddrRange(
        AddrRange(0xE0000000, size="1MiB"),
        "Private Peripheral Bus.  Base is architecturally fixed. "
        "SCS lives at offset 0xE000 within this region.",
    )

    # -- Vendor-specific (optional) --
    # 0xE0100000-0xFFFFFFFF.  Used by some vendors for proprietary
    # debug registers, OTP memory, or additional peripherals.
    vendor_ranges = VectorParam.AddrRange(
        [],
        "Vendor-specific regions.  Empty if not present.",
    )

    # -- Boot alias (optional) --
    # Many Cortex-M chips mirror the flash to address 0x00000000
    # so the vector table is readable at address 0 (where the CPU
    # fetches the initial SP and PC on reset).  When set, this
    # creates a second memory mapping of code_ranges[0] at the
    # alias address.
    # Empty list means no aliasing (e.g., flash already at 0x0,
    # or VTOR explicitly points to the real flash address).
    # One entry means the flash is aliased to that range.
    # Uses VectorParam (not Param) because AddrRange cannot be NULL.
    boot_alias_ranges = VectorParam.AddrRange(
        [],
        "Boot alias: mirrors code_ranges[0] to this address. "
        "E.g., [AddrRange(0x0, size='1MiB')] for STM32 flash alias. "
        "Empty list if no alias needed.",
    )

    # ==================================================================
    # Devices
    # ==================================================================

    # System Control Space (NVIC + SysTick + SCB).
    # This is the only device that is architecturally mandatory on
    # every Cortex-M.  Its address (0xE000E000) is fixed.
    scs = Param.MProfileSCS(
        MProfileSCS(),
        "System Control Space device (NVIC + SysTick + SCB). "
        "Configure num_irqs and priority_bits per variant.",
    )

    # Future: pluggable device list for vendor peripherals.
    # Users would append UART, SPI, GPIO, etc. devices here.
    # Each device specifies its own pio_addr; the platform wires
    # them to the memory bus automatically.
    #
    # Planned:
    #   devices = VectorParam.BasicPioDevice([],
    #       "Optional peripheral devices to attach to the bus.")
    #
    # Example future usage:
    #   platform.devices = [Pl011(pio_addr=0x40011000)]  # USART1
