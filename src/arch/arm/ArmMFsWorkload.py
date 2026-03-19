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
