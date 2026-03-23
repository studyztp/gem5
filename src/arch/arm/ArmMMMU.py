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
from m5.objects.ClockedObject import ClockedObject
from m5.objects.System import System
from m5.params import *
from m5.proxy import *


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


class StackingBarrier(ClockedObject):
    """
    Sits between the CPU icache port and the cache/memory bus.

    During M-profile exception entry, the hardware pushes an 8-word
    stack frame to SRAM via a dedicated sequencer on the data bus.
    Meanwhile, the instruction bus fetches the handler vector in
    parallel (Harvard architecture).  The CPU does not execute
    until both complete.

    This barrier models that behavior without modifying CPU models:
      - Normally transparent (requests and responses pass through).
      - When startHolding() is called (stacking begins), fetch
        requests still pass through (icache can fill from Flash)
        but responses are queued instead of forwarded to the CPU.
      - When releaseAll() is called (stacking done), held responses
        are forwarded and the CPU proceeds to execute the handler.

    Owned by ArmMMMU.  MMMU controls it from the exception
    entry/return path in m_faults.cc via startHolding()/releaseAll().

    References:
      - Cortex-M3 TRM DDI0337G Section 5.5.1 (exception entry timing)
      - Cortex-M4 TRM DDI0439D Section 3.9 (interrupt latency)
    """

    type = "StackingBarrier"
    cxx_header = "arch/arm/m_mmu.hh"
    cxx_class = "gem5::ArmISA::StackingBarrier"

    cpu_side_port = ResponsePort(
        "Port facing the CPU icache port — receives fetch requests"
    )
    mem_side_port = RequestPort(
        "Port facing the cache/memory bus — forwards fetch requests"
    )


class ArmMMMU(BaseMMU):
    """
    Pass-through MMU for M-profile (all addresses are physical).

    Delegates to ArmMTLB instances which perform identity translation.
    Contains a StackingBarrier that sits between the CPU icache port
    and the memory bus to model exception entry stall timing.
    """

    type = "ArmMMMU"
    cxx_header = "arch/arm/m_mmu.hh"
    cxx_class = "gem5::ArmISA::MMMU"

    itb = ArmMTLB()
    dtb = ArmMTLB()
    sys = Param.System(Parent.any, "System for requestor ID registration")
    stacking_barrier = Param.StackingBarrier(
        StackingBarrier(), "Stacking barrier between CPU icache and memory"
    )

    stacking_port = RequestPort(
        "Port for exception frame stacking writes and unstacking reads. "
        "Connected to the memory bus, separate from the CPU dcache port. "
        "Models the Cortex-M stacking sequencer's data bus path."
    )
