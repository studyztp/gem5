# Copyright (c) 2023 The Regents of the University of California
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

from m5.params import *
from m5.objects.Probe import ProbeListenerObject
from m5.objects import SimObject
from m5.util.pybind import *


class LooppointAnalysis(ProbeListenerObject):

    type = "LooppointAnalysis"
    cxx_header = "cpu/simple/probes/looppoint_analysis.hh"
    cxx_class = "gem5::LooppointAnalysis"

    cxx_exports = [
        PyBindMethod("startListening"),
        # start listening to committed insts
        PyBindMethod("stopListening"),
        # stop listening to committed insts
        PyBindMethod("getBBfreq"),
        # get the BB frequency map
        PyBindMethod("clearBBfreq"),
        # clear the BB frequency counts
        PyBindMethod("getlocalMostRecentPcCount"),
        # get the most recent 5 PC Count pairs
        PyBindMethod("stopFilterKernelInst"),
        PyBindMethod("startFilterKernelInst"),
        PyBindMethod("changeBBvalidAddrRange"),
        PyBindMethod("changeMarkerValidAddrRange"),
        PyBindMethod("addExcludeAddrRanges"),
        PyBindMethod("getFilteredKernelInstCount"),
        PyBindMethod("getFilteredUserInstCount"),
        PyBindMethod("clearFilteredKernelInstCount"),
        PyBindMethod("clearFilteredUserInstCount"),
    ]

    ptmanager = Param.LooppointAnalysisManager("the PcCountAnalsi manager")
    # listenerId = Param.Int(0, "this is for manager to find the listener")

    bbValidAddrRange = Param.AddrRange(
        AddrRange(start=0, end=0), "the valid insturction address range for bb"
    )

    pcCountPairValidAddrRange = Param.AddrRange(
        AddrRange(start=0, end=0),
        "the valid insturction address range for" " markers",
    )

    startListeningAtStart = Param.Bool(
        True, "if we should start listening from the start"
    )

    startKernelFilterAtStart = Param.Bool(
        True, "if we should start filtering Kernel instructions from the start"
    )

    excludeAddrRange = VectorParam.AddrRange(
        [],
        "a list of the exclude instruction address range",
    )


class LooppointAnalysisManager(SimObject):

    type = "LooppointAnalysisManager"
    cxx_header = "cpu/simple/probes/looppoint_analysis.hh"
    cxx_class = "gem5::LooppointAnalysisManager"

    cxx_exports = [
        PyBindMethod("getCounter"),
        PyBindMethod("getPcCount"),
        PyBindMethod("getBBinst"),
        PyBindMethod("getGlobalInstCounter"),
        PyBindMethod("clearGlobalInstCounter"),
        PyBindMethod("getGlobalMostRecentPC"),
        PyBindMethod("enableRaisingExitEvent"),
        PyBindMethod("disableRaisingExitEvent"),
    ]

    regionLen = Param.Int(100000000, "each region's instruction length")

    raiseExitEvent = Param.Bool(
        True,
        "if the manager should raise an exit"
        " event when the global instruction count is higher "
        "than the region length",
    )
