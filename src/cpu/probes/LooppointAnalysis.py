# Copyright (c) 2024 The Regents of the University of California
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

from m5.objects import SimObject
from m5.objects.Probe import ProbeListenerObject
from m5.params import *
from m5.util.pybind import *


class LooppointAnalysis(ProbeListenerObject):
    type = "LooppointAnalysis"
    cxx_header = "cpu/probes/looppoint_analysis.hh"
    cxx_class = "gem5::LooppointAnalysis"

    cxx_exports = [
        PyBindMethod("startListening"),
        PyBindMethod("stopListening"),
    ]

    lpmanager = Param.LooppointAnalysisManager("the PcCountAnalsi manager")
    # listenerId = Param.Int(0, "this is for manager to find the listener")

    BBValidAddrrange = Param.AddrRange(
        AddrRange(start=0, end=0), "the valid insturction address range for bb"
    )

    markerValidAddrrange = Param.AddrRange(
        AddrRange(start=0, end=0),
        "the valid insturction address range for" " markers",
    )

    BBexcludeAddrrange = VectorParam.AddrRange(
        [],
        "a list of the exclude instruction address range",
    )

    ifStartListening = Param.Bool(
        True, "if we should start listening from the start"
    )


class LooppointAnalysisManager(SimObject):
    type = "LooppointAnalysisManager"
    cxx_header = "cpu/probes/looppoint_analysis.hh"
    cxx_class = "gem5::LooppointAnalysisManager"

    cxx_exports = [
        PyBindMethod("getGlobalBBV"),
        PyBindMethod("clearGlobalBBV"),
        PyBindMethod("getGlobalInst"),
        PyBindMethod("clearGlobalInst"),
        PyBindMethod("getGlobalMostRecentPc"),
        PyBindMethod("getBBInstMap"),
        PyBindMethod("getCounter"),
    ]

    regionLen = Param.Int(100000000, "the length of the region")
