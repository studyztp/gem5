from m5.objects import SimObject
from m5.objects.Probe import ProbeListenerObject
from m5.params import *
from m5.util.pybind import *


class UserInstCounterManager(SimObject):
    type = "UserInstCounterManager"
    cxx_header = "cpu/probes/user_inst_counter.hh"
    cxx_class = "gem5::UserInstCounterManager"

    cxx_exports = [
        PyBindMethod("getUserInstCount"),
        PyBindMethod("getNoneUserInstCount"),
        PyBindMethod("resetUserInstCount"),
        PyBindMethod("resetNoneUserInstCount"),
    ]


class UserInstCounter(ProbeListenerObject):
    type = "UserInstCounter"
    cxx_header = "cpu/probes/user_inst_counter.hh"
    cxx_class = "gem5::UserInstCounter"

    if_start_listening = Param.Bool(True, "Start listening to instructions")
    counter_manager = Param.UserInstCounterManager(
        "The UserInstCounter manager"
    )
