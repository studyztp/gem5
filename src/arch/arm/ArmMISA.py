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

from m5.objects.ArmSystem import ArmRelease
from m5.objects.BaseISA import BaseISA
from m5.params import *
from m5.proxy import *


class ArmMISA(BaseISA):
    """
    ISA object for ARM M-profile (Cortex-M) processors.

    Inherits from BaseISA (NOT ArmISA) because A-profile ISA
    initialization crashes on M-profile (resetCPSR dereferences
    NULL ArmSystem pointer, initializes 750+ A-profile registers).

    M-profile is fundamentally simpler: ~24 misc registers, no CPSR
    (uses xPSR), no exception levels, no register banking, no SPSR.

    This is the same inheritance pattern as RISC-V ISA
    (src/arch/riscv/RiscvISA.py inherits from BaseISA).
    """

    type = "ArmMISA"
    cxx_class = "gem5::ArmISA::MISA"
    cxx_header = "arch/arm/m_isa.hh"

    # System reference — needed to find ArmMSystem for release/extensions.
    system = Param.System(Parent.any, "System this ISA belongs to")

    # Release for SE mode (when system is not ArmMSystem).
    # In FS mode, the release comes from ArmMSystem.releaseFS().
    release_se = Param.ArmRelease(
        ArmRelease(),
        "ARM release for SE mode (unused in FS with ArmMSystem)",
    )

    # VTOR alignment granularity.
    #
    # DDI0403E B3.2.5: VTOR bits[N-1:0] are RES0.
    # N is implementation-defined:
    #   7 = 128-byte alignment (M0/M0+/M33)
    #   9 = 512-byte alignment (M4/M7)
    vtor_align_bits = Param.UInt8(
        9,
        "Number of low VTOR bits that are RES0. "
        "7 = M0/M0+/M33 (128-byte), 9 = M4/M7 (512-byte).",
    )
