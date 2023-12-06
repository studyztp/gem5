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
from m5.util.pybind import *
from m5.objects.Probe import ProbeListenerObject
from m5.objects import SimObject


class GlobalInstTracker(SimObject):
    """This class manages global PC-count pair tracking.
    It keeps the global counters for all target PC-count pairs and raises exit
    events when a PC executed a target number of times.
    It gets called every time a PcCountTracker encounters a target PC.
    """

    type = "GlobalInstTracker"
    cxx_header = "cpu/probes/global_inst_tracker.cc"
    cxx_class = "gem5::GlobalInstTracker"

    cxx_exports = [
        PyBindMethod("clearGlobalCount"),
        PyBindMethod("updateTargetInst"),
    ]

    target = Param.UInt64("the target instruction number")


class LocalInstTracker(ProbeListenerObject):
    """This probe listener tracks the number of times a particular pc has been
    executed. It needs to be connected to a manager to track the global
    information.
    """

    type = "LocalInstTracker"
    cxx_header = "cpu/probes/global_inst_tracker.hh"
    cxx_class = "gem5::LocalInstTracker"

    cxx_exports = [
        PyBindMethod("clearLocalCount"),
        PyBindMethod("updateThreshold"),
        PyBindMethod("startListening"),
        PyBindMethod("stopListening"),
    ]

    globalCounter = Param.GlobalInstTracker("the GlobalInstTracker")
    updateThreshold = Param.UInt64(
        100, "the update frequency to global tracker"
    )
    ifListeningFromStart = Param.Bool(True, "if start listening at the start")
