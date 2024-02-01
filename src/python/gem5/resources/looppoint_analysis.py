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


import json
from pathlib import Path

import m5
from m5.objects import (
    LooppointAnalysis,
    LooppointAnalysisManager,
)


class LoopPointAnalysis:
    def __init__(
        self,
        region_length,
        if_start_listening,
        valid_address_range,
        valid_marker_range,
        exclude_address_range,
    ) -> None:
        self._region_length = region_length
        self._if_start_listening = if_start_listening
        self._valid_address_range = valid_address_range
        self._valid_marker_range = valid_marker_range
        self._exclude_address_range = exclude_address_range

        self._listener_list = []

        self._manager = LooppointAnalysisManager()
        self._manager.regionLen = self._region_length

    def setup_processor(self, processor: "AbstractProcessor"):
        for core in processor.get_cores():
            listener = LooppointAnalysis()
            listener.lpmanager = self._manager
            listener.BBValidAddrrange = self._valid_address_range
            listener.markerValidAddrrange = self._valid_marker_range
            listener.BBexcludeAddrrange = self._exclude_address_range
            listener.startListening = self._if_start_listening
            core.core.probeListener = listener
            self._listener_list.append(listener)

    def start_listening(self):
        for listener in self._listener_list:
            listener.startListening()

    def stop_listening(self):
        for listener in self._listener_list:
            listener.stopListening()
