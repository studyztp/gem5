# Copyright (c) 2024-2025 Arm Limited
# All rights reserved.
#
# The license below extends only to copyright in the software and shall
# not be construed as granting a license to any other intellectual
# property including but not limited to intellectual property relating
# to a hardware implementation of the functionality of the software
# licensed hereunder.  You may use the software subject to the license
# terms below provided that you ensure that this notice is replicated
# unmodified and in its entirety in all distributions of the software,
# modified or unmodified, in source code or in binary form.
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

from m5.objects.ArmISA import ArmISA
from m5.params import *


class ArmMISA(ArmISA):
    """ISA object for ARM M-profile (Cortex-M) processors.

    Inherits all A/R-profile configuration from ArmISA and adds M-profile
    specific parameters.  Use this SimObject instead of ArmISA when
    instantiating any Cortex-M variant (M0, M0+, M3, M4, M7, M23, M33, ...).
    """

    type = "ArmMISA"
    cxx_class = "gem5::ArmISA::MISA"
    cxx_header = "arch/arm/isa.hh"

    # VTOR alignment granularity.
    #
    # The ARMv7-M / ARMv8-M architecture specifies that VTOR bits[N-1:0] are
    # RES0 (read-as-zero, writes ignored), where N is implementation-defined
    # and depends on the number of exception entries in the vector table.
    # The architectural minimum is N=7 (128-byte alignment, M0/M0+).
    # Cortex-M4 and Cortex-M7 require N=9 (512-byte alignment).
    # Cortex-M33 requires N=7 (128-byte alignment per DDI0553).
    #
    # This parameter controls which bits are forced to zero on every write to
    # MISCREG_M_VTOR.  The mask applied is: ~((1 << vtor_align_bits) - 1).
    # Example: vtor_align_bits=9 → mask=0xFFFFFE00 (bits[8:0] always zero).
    #
    # The VTOR_t BitUnion uses Bitfield<31,0> (full width) so no shift is
    # needed when reading the register — the stored value IS the byte address
    # of the vector table.  Alignment is enforced entirely here via MISA.
    vtor_align_bits = Param.UInt8(
        9,
        "Number of low VTOR bits that are RES0 (implementation-defined "
        "alignment). 7 = 128-byte (M0/M0+/M33), 9 = 512-byte (M4/M7).",
    )
