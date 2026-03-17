from m5.objects.Workload import KernelWorkload
from m5.params import *


class ArmMFsWorkload(KernelWorkload):
    """
    Full-system workload for ARM M-profile (Cortex-M) simulation.

    Inherits KernelWorkload to get ELF loading for free.  The firmware
    binary is specified via the ``object_file`` parameter (inherited).

    Overrides several KernelWorkload defaults for bare-metal M-profile:
    - addr_check disabled (firmware may reside in flash, not DRAM)
    - no address relocation (identity load)
    - no kernel command line
    """

    type = "ArmMFsWorkload"
    cxx_header = "arch/arm/m_fs_workload.hh"
    cxx_class = "gem5::ArmISA::MFsWorkload"

    # Bare-metal firmware has no kernel command line.
    command_line = ""

    # Disable address bounds checking — M-profile firmware addresses
    # are typically in flash ranges (e.g. 0x08000000 on STM32) that
    # may not overlap with the configured DRAM range.
    addr_check = False

    # Identity load — no address masking or offset.
    load_addr_mask = 0xFFFFFFFFFFFFFFFF
    load_addr_offset = 0
