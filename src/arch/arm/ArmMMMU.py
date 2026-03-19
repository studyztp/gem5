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

from m5.objects.BaseMMU import BaseMMU
from m5.objects.BaseTLB import BaseTLB
from m5.params import *


class ArmMTLB(BaseTLB):
    """
    Pass-through TLB for M-profile (no address translation).

    M-profile has no MMU or TLB — all addresses are physical.  This
    class satisfies the BaseTLB interface by returning VA == PA for
    every translation request.

    TODO: When the optional MPU (Memory Protection Unit) is modelled,
    access-permission checks can be added here.
    """

    type = "ArmMTLB"
    cxx_header = "arch/arm/m_mmu.hh"
    cxx_class = "gem5::ArmISA::MTLB"

    entry_type = "unified"


class ArmMMMU(BaseMMU):
    """
    Pass-through MMU for M-profile (all addresses are physical).

    Delegates to ArmMTLB instances which perform identity translation.
    """

    type = "ArmMMMU"
    cxx_header = "arch/arm/m_mmu.hh"
    cxx_class = "gem5::ArmISA::MMMU"

    itb = ArmMTLB()
    dtb = ArmMTLB()
