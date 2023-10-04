# Copyright (c) 2022 The Regents of the University of California
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

from gem5.utils.override import overrides
from gem5.components.boards.mem_mode import MemMode

from m5.util import warn

from gem5.components.processors.base_cpu_processor import BaseCPUProcessor
from gem5.components.processors.switchable_processor import SwitchableProcessor
from gem5.components.processors.simple_core import SimpleCore
from gem5.components.processors.cpu_types import CPUTypes
from gem5.components.boards.abstract_board import AbstractBoard
from gem5.components.boards.mem_mode import MemMode
from gem5.isas import ISA
from .riscvmatched_core import U74Core


class U74SwitchProcessor(SwitchableProcessor):
    """
    A U74Processor contains a number of cores of U74Core.
    """

    def __init__(
        self,
        is_fs: bool,
    ) -> None:
        self._cpu_type = CPUTypes.ATOMIC
        self._start_key = "start"
        self._switch_key = "switch"
        self._current_is_start = True

        if is_fs:
            num_cores = 4
        else:
            num_cores = 1

        starting_cores = [
            SimpleCore(cpu_type=CPUTypes.ATOMIC, core_id=i, isa=ISA.RISCV)
            for i in range(num_cores)
        ]
        switching_cores = [U74Core(core_id=i) for i in range(num_cores)]

        switchable_cores = {
            self._start_key: starting_cores,
            self._switch_key: switching_cores,
        }

        super().__init__(
            switchable_cores=switchable_cores, starting_cores=self._start_key
        )

    @overrides(SwitchableProcessor)
    def incorporate_processor(self, board: AbstractBoard) -> None:
        super().incorporate_processor(board=board)

        if (
            board.get_cache_hierarchy().is_ruby()
            and self._mem_mode == MemMode.ATOMIC
        ):
            warn(
                "Using an atomic core with Ruby will result in "
                "'atomic_noncaching' memory mode. This will skip caching "
                "completely."
            )
            self._mem_mode = MemMode.ATOMIC_NONCACHING
        board.set_mem_mode(self._mem_mode)

    def switch(self):
        """Switches to the "switched out" cores."""
        if self._current_is_start:
            self._cpu_type = CPUTypes.MINOR
            self.switch_to_processor(self._switch_key)
        else:
            self._cpu_type = CPUTypes.ATOMIC
            self.switch_to_processor(self._start_key)

        self._current_is_start = not self._current_is_start
