# Copyright (c) 2021 The Regents of the University of California
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

from typing import Generator, Optional
import m5.stats
from ..components.processors.abstract_processor import AbstractProcessor
from ..components.processors.switchable_processor import SwitchableProcessor
from ..resources.resource import SimpointResource
from gem5.resources.looppoint import Looppoint
from m5.util import warn
from pathlib import Path
import json

"""
In this package we store generators for simulation exit events.
"""


def warn_default_decorator(gen: Generator, type: str, effect: str):
    """A decortator for generators which will print a warning that it is a
    default generator.
    """

    def wrapped_generator(*args, **kw_args):
        warn(
            f"No behavior was set by the user for {type}."
            f" Default behavior is {effect}."
        )
        for value in gen(*args, **kw_args):
            yield value

    return wrapped_generator


def exit_generator():
    """
    A default generator for an exit event. It will return True, indicating that
    the Simulator run loop should exit.
    """
    while True:
        yield True


def switch_generator(processor: AbstractProcessor):
    """
    A default generator for a switch exit event. If the processor is a
    SwitchableProcessor, this generator will switch it. Otherwise nothing will
    happen.
    """
    is_switchable = isinstance(processor, SwitchableProcessor)
    while True:
        if is_switchable:
            yield processor.switch()
        else:
            yield False


def dump_reset_generator():
    """
    A generator for doing statstic dump and reset. It will reset the simulation
    statistics and then dump simulation statistics.
    The Simulation run loop will continue after executing the behavior of the
    generator.
    """
    while True:
        m5.stats.dump()
        m5.stats.reset()
        yield False


def save_checkpoint_generator(checkpoint_dir: Optional[Path] = None):
    """
    A generator for taking a checkpoint. It will take a checkpoint with the
    input path and the current simulation Ticks.
    The Simulation run loop will continue after executing the behavior of the
    generator.
    """
    if not checkpoint_dir:
        from m5 import options

        checkpoint_dir = Path(options.outdir)
    while True:
        m5.checkpoint((checkpoint_dir / f"cpt.{str(m5.curTick())}").as_posix())
        yield False


def reset_stats_generator():
    """
    This generator resets the stats every time it is called. It does not dump
    the stats before resetting them.
    """
    while True:
        m5.stats.reset()
        yield False


def dump_stats_generator():
    """
    This generator dumps the stats every time it is called.
    """
    while True:
        m5.stats.dump()
        yield False


def skip_generator():
    """
    This generator does nothing when on the exit event.
    The simulation will continue after this generator.
    """
    while True:
        yield False


def simpoints_save_checkpoint_generator(
    checkpoint_dir: Path, simpoint: SimpointResource
):
    """
    A generator for taking multiple checkpoints for SimPoints. It will save the
    checkpoints in the checkpoint_dir path with the SimPoints' index.
    The Simulation run loop will continue after executing the behavior of the
    generator until all the SimPoints in the simpoint_list has taken a
    checkpoint.
    """
    simpoint_list = simpoint.get_simpoint_start_insts()
    count = 0
    last_start = -1
    while True:
        m5.checkpoint((checkpoint_dir / f"cpt.SimPoint{count}").as_posix())
        last_start = simpoint_list[count]
        count += 1
        # When the next SimPoint starting instruction is the same as the last
        # one, it will take a checkpoint for it with index+1. Because of there
        # are cases that the warmup length is larger than multiple SimPoints
        # starting instructions, then they might cause duplicates in the
        # simpoint_start_ints.
        while (
            count < len(simpoint_list) and last_start == simpoint_list[count]
        ):
            m5.checkpoint((checkpoint_dir / f"cpt.SimPoint{count}").as_posix())
            last_start = simpoint_list[count]
            count += 1
        # When there are remaining SimPoints in the list, let the Simulation
        # loop continues, otherwise, exit the Simulation loop.
        if count < len(simpoint_list):
            yield False
        else:
            yield True


def looppoint_save_checkpoint_generator(
    checkpoint_dir: Path,
    looppoint: Looppoint,
    update_relatives: bool = True,
    exit_when_empty: bool = True,
):
    """
    A generator for taking a checkpoint for LoopPoint. It will save the
    checkpoints in the checkpoint_dir path with the Region id.
    (i.e. "cpt.Region10) It only takes a checkpoint if the current PC Count
    pair is a significant PC Count Pair. This is determined in the LoopPoint
    module. The simulation loop continues after exiting this generator.
    :param checkpoint_dir: where to save the checkpoints
    :param loopoint: the looppoint object used in the configuration script
    :param update_relative: if the generator should update the relative count
    information in the output json file, then it should be True. It is default
    as True.
    :param exit_when_empty: if the generator should exit the simulation loop if
    all PC paris have been discovered, then it should be True. It is default as
    True.
    """
    if exit_when_empty:
        total_pairs = len(looppoint.get_targets())
    else:
        total_pairs = -1
        # it will never equal to 0 if exit_when_empty is false

    while total_pairs != 0:
        region = looppoint.get_current_region()
        # if it is a significant PC Count pair, then the get_current_region()
        # will return an integer greater than 0. By significant PC Count pair,
        # it means the PC Count pair that indicates where to take the
        # checkpoint at. This is determined in the LoopPoint module.
        if region:
            if update_relatives:
                looppoint.update_relatives_counts()
            m5.checkpoint((checkpoint_dir / f"cpt.Region{region}").as_posix())
        total_pairs -= 1
        yield False

    yield True


def smarts_generator(
    k: int, U: int, W: int, json_file_path: Path, processor: AbstractProcessor
):
    """
    :param k: the systematic sampling interval. Each interval simulation k*U
    instructions. The interval includes the fastforwarding part, detailed
    warmup part, and the detail simulation part.
    :param U: sampling unit size. The instruction length in each unit.
    :param W: the length of the detailed warmup part.

    Each interval instruction length is k*U.
    The warmup part starts at (k-1)*U-W
    The detailed simulation part starts at (k-1)*U

    This exit generator only works with SwitchableProcessor.
    When it reaches to the start of the detailed warmup part, it dumps and
    resets the stats; then it switchs the core type and schedule for the end
    of the warmup part and the end of the interval.
    When it reaches to the end of the detailed warmup part, it dumps and resets
    the stats.
    When it reaches to the end of the detailed simulation, it dumps and resets
    the stats; then it switches the core type and schedule for the start of the
    next detailed warmup part.
    """
    is_switchable = isinstance(processor, SwitchableProcessor)
    warmup_start = U * (k - 1) - W
    warmup_plus_detailed = U + W
    counter = 0
    last_warmup_start_inst_count = 0
    last_detailed_start_inst_count = 0
    if json_file_path.exists():
        with open(json_file_path.as_posix()) as file:
            default_data = json.load(file)
    else:
        default_data = {}
    default_data["sample-data"] = {}
    info_json = default_data["sample-data"]

    while is_switchable:
        # reached to the warmup start
        info_json[counter] = {}
        print(f"curTick is {m5.curTick()}")
        print("got to warmup start\n")
        print("now dump and reset stats\n")
        # dump stats
        m5.stats.dump()
        info_json[counter]["warmup-start-tick"] = m5.curTick()
        inst = processor.get_cores()[0].core.totalInsts()
        info_json[counter]["warmup-start-inst-count"] = (
            inst - last_warmup_start_inst_count
        )
        last_warmup_start_inst_count = inst
        # reset stats
        m5.stats.reset()
        print("switch core type")
        # switch core type
        processor.switch()
        print(
            "now schedule for end of warmup and start of detailed simluation\n"
        )
        # schedule for warmup end
        # schedule for detailed simulation end
        processor.get_cores()[0]._set_simpoint([W, warmup_plus_detailed], True)
        print("fall back to simulation\n")
        # fall back to simualtion
        yield False

        # reached warmup end
        print(f"curTick is {m5.curTick()}")
        print("got to detail simulation start\n")
        print("now dump and reset m5 stats\n")
        # dump stats
        m5.stats.dump()
        info_json[counter]["detail-start-tick"] = m5.curTick()
        inst = processor.get_cores()[0].core.totalInsts()
        info_json[counter]["detail-start-inst-count"] = (
            inst - last_detailed_start_inst_count
        )
        last_detailed_start_inst_count = inst
        # reset stats
        m5.stats.reset()
        print("fall back to simulation\n")
        # fall back to simulation
        yield False

        # reached end of detailed simulation
        print(f"curTick is {m5.curTick()}")
        print("got to end of detail simulation\n")
        print("now dump and reset stats\n")
        # dump stats
        m5.stats.dump()
        info_json[counter]["detail-end-tick"] = m5.curTick()
        inst = processor.get_cores()[0].core.totalInsts()
        info_json[counter]["detail-end-inst-count"] = (
            inst - last_detailed_start_inst_count
        )
        with open(json_file_path.as_posix(), "w") as file:
            json.dump(default_data, file, indent=4)
        # reset stats
        m5.stats.reset()
        # switch core type
        print("switch core type\n")
        processor.switch()
        print(
            "now schedule for next warmup start and detail simulation start\n"
        )
        # schedule for the next start of warmup
        print("schedule for the next start of warmup\n")
        processor.get_cores()[0]._set_simpoint([warmup_start], True)
        print("increase n counter\n")
        # increment sample counter
        counter += 1
        print("switch core type to functional core type")
        print("fall back to simulation\n")
        yield False
