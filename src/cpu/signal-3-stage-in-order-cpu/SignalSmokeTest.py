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

from m5.objects.ClockedObject import ClockedObject
from m5.params import *


class SignalSmokeTest(ClockedObject):
    """S0 smoke test for the Signal/Latch primitive.

    Builds a counter -> Latch<int> -> printer pipeline and runs it
    for `num_cycles`.  Optionally schedules a one-shot async trigger
    inside one of those cycles to validate the mid-cycle window
    classification (Rev 2 issue #2).
    """

    type = "SignalSmokeTest"
    cxx_header = "cpu/signal-3-stage-in-order-cpu/signal_smoke_test.hh"
    cxx_class = "gem5::signal3::SignalSmokeTest"

    num_cycles = Param.Cycles(
        100, "Number of clock cycles to run before exiting"
    )

    run_async_test = Param.Bool(
        True,
        "Schedule a one-shot async trigger to validate "
        "SignalGraph::isWithinCycle at a mid-cycle tick",
    )

    async_trigger_cycle = Param.Cycles(
        10, "Cycle whose interior we'll schedule the async trigger in"
    )

    async_trigger_offset_ticks = Param.Tick(
        100,
        "Ticks past the start of async_trigger_cycle at which the "
        "async event fires.  Must be < clock period (1 cycle = "
        "default 1000 ticks at 1 GHz).",
    )
