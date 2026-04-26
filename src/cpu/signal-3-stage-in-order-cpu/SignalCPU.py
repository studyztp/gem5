# Copyright (c) 2026 Zhantong Qiu, University of California, Davis
# and Cornell University
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

from m5.objects.BaseCPU import BaseCPU
from m5.params import *
from m5.proxy import Self


class SignalCPU(BaseCPU):
    """Signal-driven 3-stage in-order CPU.

    Day-one knobs only (per DESIGN.md §6, plan Rev 2).  No
    `mispredict_flush_cycles`, `taken_branch_redirect_delay`, etc. —
    those are added in S6 only on data-driven demand.
    """

    type = "SignalCPU"
    cxx_header = "cpu/signal-3-stage-in-order-cpu/signal_cpu.hh"
    cxx_class = "gem5::signal3::SignalCPU"

    @classmethod
    def memory_mode(cls):
        return "timing"

    @classmethod
    def support_take_over(cls):
        return False

    # ---- Cortex-M reference knobs (TRM-grounded defaults) ----

    pfu_fifo_words = Param.Unsigned(
        3, "PFU prefetch FIFO depth (32-bit words). Cortex-M3 TRM §1.4."
    )

    halt_addr = Param.Addr(
        0,
        "Architectural PC at which the CPU halts the simulation "
        "(typically the binary's `halt: b halt` infinite loop).  "
        "0 means run forever.",
    )

    icache_port = RequestPort("Instruction-side cache/AHB port")
    dcache_port = RequestPort("Data-side cache/AHB port")

    @classmethod
    def require_caches(cls):
        return False
