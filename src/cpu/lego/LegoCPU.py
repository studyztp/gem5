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
from m5.proxy import *
from m5.SimObject import SimObject

# ---------- Stage Functions (base classes) ----------


class LegoStageFunction(SimObject):
    """Base class for all Lego pipeline stage functions."""

    type = "LegoStageFunction"
    abstract = True
    cxx_header = "cpu/lego/stage_function.hh"
    cxx_class = "gem5::StageFunction"

    funcName = Param.String("", "Unique name for connection wiring")


class LegoStageFunctionWithPort(LegoStageFunction):
    """Base class for stage functions that interact with memory ports."""

    type = "LegoStageFunctionWithPort"
    abstract = True
    cxx_header = "cpu/lego/stage_function.hh"
    cxx_class = "gem5::StageFunctionWithPort"


class LegoStageFunctionTranslation(LegoStageFunction):
    """Base class for stage functions that use MMU translation."""

    type = "LegoStageFunctionTranslation"
    abstract = True
    cxx_header = "cpu/lego/stage_function.hh"
    cxx_class = "gem5::StageFunctionTranslation"


# ---------- Fetch Functions ----------


class FetchAddressGen(LegoStageFunctionTranslation):
    """Takes PC, translates VA->PA via MMU, outputs fetch address."""

    type = "FetchAddressGen"
    cxx_header = "cpu/lego/fetch_functions.hh"
    cxx_class = "gem5::FetchAddressGen"

    fetchWidth = Param.Unsigned(
        16, "Fetch width in bytes (must be power of 2)"
    )


class FetchMemRequest(LegoStageFunctionWithPort):
    """Sends icache request, receives response, outputs fetch line."""

    type = "FetchMemRequest"
    cxx_header = "cpu/lego/fetch_functions.hh"
    cxx_class = "gem5::FetchMemRequest"


# ---------- Execute Functions ----------


class InstructionDecode(LegoStageFunction):
    """Decodes fetched bytes into a decoded instruction."""

    type = "InstructionDecode"
    cxx_header = "cpu/lego/execute_functions.hh"
    cxx_class = "gem5::InstructionDecode"


class ALUExecute(LegoStageFunction):
    """Executes ALU instructions."""

    type = "ALUExecute"
    cxx_header = "cpu/lego/execute_functions.hh"
    cxx_class = "gem5::ALUExecute"


class PCUpdate(LegoStageFunction):
    """Updates PC, produces redirect on branch."""

    type = "PCUpdate"
    cxx_header = "cpu/lego/execute_functions.hh"
    cxx_class = "gem5::PCUpdate"


# ---------- SubStage ----------


class LegoSubStage(SimObject):
    """An ordered sequence of stage functions."""

    type = "LegoSubStage"
    cxx_header = "cpu/lego/sub_stage.hh"
    cxx_class = "gem5::SubStage"

    functions = VectorParam.LegoStageFunction(
        [], "Ordered list of stage functions"
    )


# ---------- Stage ----------


class LegoStage(SimObject):
    """A pipeline stage (one cycle boundary)."""

    type = "LegoStage"
    cxx_header = "cpu/lego/stage.hh"
    cxx_class = "gem5::Stage"

    subStages = VectorParam.LegoSubStage([], "Ordered list of substages")


# ---------- LegoCPU ----------


class LegoCPU(BaseCPU):
    """Lego CPU: a modular in-order CPU model with reactive
    port-based dataflow pipeline.
    """

    type = "LegoCPU"
    cxx_header = "cpu/lego/lego_cpu.hh"
    cxx_class = "gem5::LegoCPU"

    @classmethod
    def memory_mode(cls):
        return "timing"

    stages = VectorParam.LegoStage([], "Ordered list of pipeline stages")

    # Connection wiring: "srcFunc.portName:dstFunc.portName"
    # srcFunc/dstFunc are funcName values set on each function.
    connections = VectorParam.String(
        [], "Port connections as 'srcFunc.port:dstFunc.port' strings"
    )
