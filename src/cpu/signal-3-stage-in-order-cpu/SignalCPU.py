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


def build_opclass_latencies(overrides):
    """Build the full Num_OpClass-length latency vector from a
    sparse dict of overrides.  Unspecified OpClasses default to 1.

    Args:
        overrides: dict mapping `m5.objects.FuncUnit.OpClass` enum
            values (or their string names) to integer cycle counts.

    Returns:
        A list of integers of length `Num_OpClass`, suitable for
        assignment to `SignalCPU.opclass_latencies`.

    Example:
        from m5.objects.FuncUnit import OpClass
        from m5.objects.SignalCPU import build_opclass_latencies
        cpu.opclass_latencies = build_opclass_latencies({
            OpClass("SimdFloatDiv"):  14,
            OpClass("SimdFloatSqrt"): 14,
        })

    Helper lives at module scope (not class scope) because the
    gem5 SimObject metaclass treats class-level non-classmethod
    attributes as Param declarations.
    """
    from m5.objects.FuncUnit import OpClass

    # `OpClass.map` is a {name_str: int_index} dict (see
    # m5/params/enum_params.py MetaEnum), and `OpClass(...).value`
    # is the string name.  Build the latency vector indexed by
    # that integer.
    n = len(OpClass.map)
    out = [1] * n
    for op, lat in overrides.items():
        if isinstance(op, str):
            name = op
        elif hasattr(op, "value"):
            name = op.value
        else:
            name = str(op)
        if name not in OpClass.map:
            raise ValueError(
                f"build_opclass_latencies: unknown OpClass '{name}' "
                f"(expected one of {sorted(OpClass.map.keys())[:5]}...)"
            )
        out[OpClass.map[name]] = lat
    return out


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

    pfu_max_outstanding_fetches = Param.Unsigned(
        2,
        "Maximum simultaneously-outstanding fetches from F to the "
        "icache port (in-flight + pending-to-issue).  Models AHB-Lite "
        "back-pressure on the I-bus: at any HCLK at most 1 transfer "
        "is in address phase + 1 in data phase, so a 3rd transfer "
        "must wait for the 1st to complete before its address phase "
        "can begin.  Default 2 = strict AHB-Lite.  Set to 0 to "
        "disable the cap (only the FIFO depth gates new issues).",
    )

    halt_addr = Param.Addr(
        0,
        "Architectural PC at which the CPU halts the simulation "
        "(typically the binary's `halt: b halt` infinite loop).  "
        "0 means run forever.",
    )

    icache_port = RequestPort("Instruction-side cache/AHB port")
    dcache_port = RequestPort("Data-side cache/AHB port")

    # Per-OpClass execute latency in cycles.  Empty default keeps
    # the C++ side at all-1s; non-empty must have length
    # `enums::Num_OpClass` (see gem5/src/cpu/op_class.hh).  Use the
    # `build_opclass_latencies()` helper at module scope to
    # construct the full vector from a sparse {OpClass: cycles}
    # dict.  ARM M-profile overrides (SimdFloatDiv = 14,
    # SimdFloatSqrt = 14, per DDI0439B Table 3-1) are applied in
    # ArmMSignalCPU in arch/arm/ArmMCPU.py.
    opclass_latencies = VectorParam.Cycles(
        [],
        "Per-OpClass execute latency in cycles.  Empty -> all 1.  "
        "When non-empty, must be length enums::Num_OpClass; entries "
        "indexed by the OpClass enum.  Looked up by "
        "AluFunctionUnit::latencyFor() to pick per-inst cycle counts.",
    )

    @classmethod
    def require_caches(cls):
        return False
