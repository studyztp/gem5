from m5.objects.SimpleMemory import SimpleMemory
from m5.params import *


class PipelinedSimpleMemory(SimpleMemory):
    """SimpleMemory with AHB-style address/data phase overlap and an
    optional read buffer modelling a flash controller's internal read
    register.

    Accepts up to ``max_outstanding`` concurrent requests (instead of
    blocking on ``isBusy`` like the base class).  Consecutive reads within
    the same aligned ``read_buffer_size`` block are served at
    ``buffer_hit_cycles`` instead of the full ``latency``.
    """

    type = "PipelinedSimpleMemory"
    cxx_header = "mem/pipelined_simple_mem.hh"
    cxx_class = "gem5::memory::PipelinedSimpleMemory"

    max_outstanding = Param.Unsigned(
        2,
        "Maximum number of outstanding requests "
        "(models AHB address/data phase overlap)",
    )
    address_phase_cycles = Param.Cycles(
        1,
        "Minimum cycles between accepting consecutive requests "
        "(models AHB address phase occupancy)",
    )
    read_buffer_size = Param.Unsigned(
        0,
        "Size of internal read buffer in bytes (0 = disabled). "
        "Models the flash controller's N-bit read register: reads within "
        "the same aligned block are served at buffer_hit_cycles instead "
        "of full latency. Must be 0 or a power of 2.",
    )
    buffer_hit_cycles = Param.Cycles(
        1,
        "Latency in cycles for a read buffer hit "
        "(models bus transfer time without memory access)",
    )
