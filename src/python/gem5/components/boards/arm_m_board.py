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
Board for ARM Cortex-M (M-profile) full-system simulation.

== Design decisions ==

1. ArmMBoard inherits from ArmMSystem, NOT from AbstractBoard.

   AbstractBoard (used by ArmBoard for A-profile) expects:

     - AbstractMemorySystem: a single memory object whose size is
       queried and ranges set by the board.  M-profile memory is
       fundamentally different: the platform defines multiple disjoint
       memory blocks (SRAM1, SRAM2, CCM at different addresses), each
       potentially a different type of memory with different latency.
       There is no single "memory size" — the memory topology IS the
       platform definition.

     - AbstractCacheHierarchy (required): always present on A-profile.
       On M-profile, caches are optional and variant-specific:
         - Cortex-M4: no architectural cache (STM32F4 has ART accelerator)
         - Cortex-M7: has I-cache and D-cache
         - Cortex-M33: has optional I-cache
       Our board accepts cache_hierarchy as an optional parameter.

     - KernelDiskWorkload: for Linux boot with disk images and DTB.
       M-profile runs bare-metal firmware loaded as an ELF — no disk,
       no DTB, no Linux kernel.  We use MFsWorkload (from Step 5).

   Rather than fighting AbstractBoard's interface with dummy
   implementations, ArmMBoard directly inherits ArmMSystem and
   handles bus wiring itself.  This is simpler, honest about what
   M-profile needs, and can be refactored to use AbstractBoard
   later if the interface evolves.

2. Memories are provided by the user, NOT created by the board.

   The board does NOT decide what memory type (SimpleMemory, etc.)
   or latency backs each address range.  Different chips have very
   different memory characteristics:
     - Flash: may have wait states, prefetch buffers, ART accelerator
     - CCM SRAM: zero-wait-state, but no DMA access
     - External RAM: high latency, may need FMC controller model

   The user (or a pre-built convenience function) creates memory
   SimObjects with the appropriate type and latency, and passes
   them to the board.  The board just wires them to the bus.

3. Peripheral address space is NOT backed by SimpleMemory.

   The peripheral region (0x40000000-0x5FFFFFFF) contains devices
   (UART, SPI, I2C, GPIO), not memory.  Each device is a PioDevice
   that handles its own address range.  Devices plug into the
   platform and connect to the bus through the platform's device
   infrastructure.  Unmapped addresses in the peripheral space hit
   the BadAddr responder (bus error), which is the correct hardware
   behavior for accessing a non-existent peripheral.

== Memory bus topology ==

Without cache hierarchy:

    CPU icache_port ──┐
    CPU dcache_port ──┤
                      └──> membus ──> [memories..., SCS, devices...]

With cache hierarchy:

    CPU icache_port ──┐
    CPU dcache_port ──┤
                      └──> cache ──> membus ──> [memories..., SCS, devices...]

== Usage ==

Minimal (no cache, user-provided memories):

    from m5.objects import SimpleMemory, ArmMAtomicSimpleCPU
    from m5.params import AddrRange
    from gem5.prebuilt.cortexm import STM32F405Platform
    from gem5.components.boards.arm_m_board import ArmMBoard

    platform = STM32F405Platform()
    cpu = ArmMAtomicSimpleCPU()

    # User creates memories — full control over type and latency
    memories = [
        SimpleMemory(range=AddrRange(0x08000000, size='1MiB'),
                     latency='30ns'),   # Flash
        SimpleMemory(range=AddrRange(0x20000000, size='112KiB'),
                     latency='5ns'),    # SRAM1
        SimpleMemory(range=AddrRange(0x2001C000, size='16KiB'),
                     latency='5ns'),    # SRAM2
        SimpleMemory(range=AddrRange(0x10000000, size='64KiB'),
                     latency='5ns'),    # CCM
    ]

    board = ArmMBoard(
        platform=platform, cpu=cpu, memories=memories,
        clk_freq="168MHz",
    )
    board.set_workload("path/to/firmware.elf")

Using pre-built convenience method:

    platform = STM32F405Platform()
    board = ArmMBoard(
        platform=platform,
        cpu=ArmMAtomicSimpleCPU(),
        memories=platform.default_memories(),
        clk_freq="168MHz",
    )
"""

from m5.objects import (
    ArmMFsWorkload,
    ArmMSystem,
    BadAddr,
    SrcClockDomain,
    SystemXBar,
    VoltageDomain,
)


class ArmMBoard(ArmMSystem):
    """
    Board for Cortex-M full-system simulation.

    Takes a platform, CPU, and user-provided memories, wires them
    to a SystemXBar, and sets up bare-metal firmware execution.

    After construction, call set_workload() with the firmware ELF
    path, then pass this board to gem5's Simulator.
    """

    def __init__(
        self,
        platform,
        cpu,
        memories,
        clk_freq="168MHz",
        cache_hierarchy=None,
    ):
        """
        Construct the board and wire all components.

        After this returns, the board is fully connected and ready
        for set_workload().  No manual port wiring needed.

        Args:
            platform: An ArmMPlatform instance defining the SCS
                configuration and (optionally) peripheral devices.
                Examples: STM32F405Platform(), STM32G474REPlatform().

            cpu: An M-profile CPU SimObject instance.  Must be one of
                ArmMAtomicSimpleCPU, ArmMTimingSimpleCPU, or
                ArmMMinorCPU.

            memories: A list of memory SimObjects (e.g., SimpleMemory)
                with their address ranges already configured.  Each
                memory is connected to the system bus.  The board does
                NOT create memories — the user (or a convenience
                function like platform.default_memories()) controls
                the memory type and latency.

            clk_freq: Clock frequency string (e.g., "168MHz").
                Applied to the system clock domain.

            cache_hierarchy: Optional cache hierarchy SimObject to
                place between the CPU and the memory bus.
                - None (default): CPU connects directly to the bus.
                  Common for Cortex-M0/M4 which have no arch cache.
                - Provided: CPU connects to the cache, which connects
                  to the bus.  Use for Cortex-M7 (I/D-cache),
                  Cortex-M33 (optional I-cache), or STM32F4 ART
                  accelerator models.
                Must expose cpu_side_ports and mem_side_ports.
        """
        super().__init__()

        # ---- CPUID from platform ----
        # The platform defines which core variant it uses (M0/M3/M4/M7)
        # and provides the corresponding CPUID value via cpuid() method.
        # Pass it to ArmMSystem (our parent) so MISA can initialize
        # MISCREG_M_CPUID during construction.
        self.cpuid = platform.cpuid()

        # ---- Store references ----
        self._platform = platform
        self._cache_hierarchy = cache_hierarchy

        # ---- Clock and voltage domain ----
        # M-profile chips typically run a single clock domain.
        self.voltage_domain = VoltageDomain(voltage="1.0V")
        self.clk_domain = SrcClockDomain(
            clock=clk_freq,
            voltage_domain=self.voltage_domain,
        )

        # ---- Platform ----
        # Attach as a child SimObject so the SCS (and future devices)
        # are part of the SimObject hierarchy.
        self.platform = platform

        # ---- Memory mode ----
        # Must be set before CPU creation.  AtomicSimpleCPU uses
        # "atomic" mode; TimingSimpleCPU and MinorCPU use "timing".
        # This determines how the memory system handles requests.
        # (See configs/example/arm/baremetal.py:111-113)
        self.mem_mode = cpu.memory_mode()

        # ---- CPU ----
        # Attach as a child SimObject.  gem5 needs the CPU in the
        # hierarchy for thread creation, ISA init, and interrupt
        # controller setup.
        self.cpu = cpu

        # Assign CPU to this system's clock domain and give it an ID.
        # (See src/cpu/CpuCluster.py:65-80 for the full setup pattern)
        self.cpu.clk_domain = self.clk_domain
        self.cpu.cpu_id = 0
        self.cpu.socket_id = 0

        # Create threads, ISA instances, and decoders.
        # BaseCPU.createThreads() (src/cpu/BaseCPU.py:231) creates
        # ISA instances from ArchISA (ArmMISA) and decoders from
        # ArchDecoder (ArmMDecoder) for each thread.  Without this
        # call, the CPU has 0 ISAs and will fatal at instantiate().
        self.cpu.createThreads()

        # Create the interrupt controller (MProfileInterrupts).
        # This is separate from createThreads() — it creates the
        # per-thread interrupt controller objects that the CPU uses
        # to check for pending interrupts.
        # (See src/cpu/CpuCluster.py:74)
        self.cpu.createInterruptController()

        # ---- Memory bus ----
        # A single SystemXBar connects all memories, devices, and
        # the CPU.  M-profile SoCs have a bus matrix (AHB); one
        # SystemXBar is an appropriate model.
        #
        # BadAddr responder catches accesses to unmapped addresses
        # (e.g., reads to a non-existent peripheral) and returns a
        # bus error instead of hanging the simulation.
        self.membus = SystemXBar()
        self.membus.badaddr_responder = BadAddr()
        self.membus.default = self.membus.badaddr_responder.pio

        # ---- System port ----
        # Required by gem5: used for functional accesses such as
        # ELF loading by the workload and debugger memory reads.
        self.system_port = self.membus.cpu_side_ports

        # ---- Attach user-provided memories to the bus ----
        # Each memory covers a specific address range (flash bank,
        # SRAM block, CCM, etc.).  The user controls the memory type
        # and latency — the board just wires them.
        self.mem_ranges = []
        for i, mem in enumerate(memories):
            # Attach as named children so they show in config output
            # (e.g., "mem_0", "mem_1", ...).
            setattr(self, f"mem_{i}", mem)
            mem.port = self.membus.mem_side_ports
            self.mem_ranges.append(mem.range)

        # ---- Wire the SCS device to the bus ----
        # The SCS (NVIC + SysTick + SCB) is a BasicPioDevice at the
        # architecturally fixed address 0xE000E000.  Firmware
        # accesses it via normal load/store instructions.
        # The SCS is a child of the platform (not the board) to
        # avoid a cycle in the SimObject hierarchy.
        # MProfileSCS::init() registers itself with ArmMSystem via
        # setSCS() — no need to set self.scs here.
        self.platform.scs.pio = self.membus.mem_side_ports

        # ---- Wire CPU to bus (through optional cache hierarchy) ----
        self._connect_cpu()

    def _connect_cpu(self):
        """
        Wire the CPU's memory ports to the bus.

        Two modes:
          1. No cache: CPU ports connect directly to the membus.
             This is the common case for Cortex-M0/M4.

          2. With cache: CPU ports connect to the cache hierarchy,
             which connects to the membus.  The cache hierarchy must
             expose cpu_side_ports (for CPU) and mem_side_ports (for
             bus), following the standard gem5 cache interface.
        """
        if self._cache_hierarchy is not None:
            # CPU --> cache hierarchy --> membus
            self.cache_hierarchy = self._cache_hierarchy
            self.cpu.icache_port = self.cache_hierarchy.cpu_side_ports
            self.cpu.dcache_port = self.cache_hierarchy.cpu_side_ports
            self.cache_hierarchy.mem_side_ports = self.membus.cpu_side_ports
        else:
            # CPU --> membus directly (no caching)
            self.cpu.icache_port = self.membus.cpu_side_ports
            self.cpu.dcache_port = self.membus.cpu_side_ports

    def set_workload(self, firmware_path):
        """
        Set the bare-metal firmware ELF to load and execute.

        The workload uses MFsWorkload (from Step 5) which:
          1. Loads the ELF sections into the appropriate memories
             (flash, SRAM, etc.) based on their load addresses.
          2. Invokes MProfileReset to read the initial SP from
             VTOR+0 and the Reset_Handler address from VTOR+4.
          3. Activates the CPU thread to begin execution.

        Args:
            firmware_path: Path to an ARM Cortex-M ELF binary
                (e.g., compiled with arm-none-eabi-gcc).
        """
        self.workload = ArmMFsWorkload(object_file=firmware_path)
