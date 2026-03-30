from m5.objects.AbstractMemory import AbstractMemory
from m5.params import *


class PipelinedSimpleMemory(AbstractMemory):
    """Multi-ported pipelined memory with per-port read buffers and priority.

    Models a flash controller with independent ICode/DCode read ports,
    AHB-style address/data phase overlap, per-port read buffers, and
    priority-based scheduling (e.g., STM32G4 flash controller where
    DCode has priority over ICode [RM0440 §3.3.4]).

    Uses VectorResponsePort so multiple buses can connect to the same
    physical memory.  Each port has its own read buffer state, buffer
    size, and priority level.

    Per-port configuration is via VectorParam indexed by port ID:
      port_priority[0] = ICode priority, port_priority[1] = DCode priority
      port_read_buffer_size[0] = ICode buffer bytes, etc.
    If the vector has fewer entries than ports, the last value is reused.
    """

    type = "PipelinedSimpleMemory"
    cxx_header = "mem/pipelined_simple_mem.hh"
    cxx_class = "gem5::memory::PipelinedSimpleMemory"

    port = VectorResponsePort(
        "Vector of response ports (one per bus connection)"
    )

    # Params from SimpleMemory that we need since we no longer inherit it
    latency = Param.Latency("30ns", "Request to response latency")
    latency_var = Param.Latency("0ns", "Request to response latency variance")
    bandwidth = Param.MemoryBandwidth(
        "12.8GiB/s", "Combined read and write bandwidth"
    )

    # Shared flash pipeline params
    max_outstanding = Param.Unsigned(
        2,
        "Maximum total outstanding requests across all ports "
        "(models flash array pipeline depth)",
    )
    max_per_port = Param.Unsigned(
        1,
        "Maximum outstanding requests per port under contention. "
        "When multiple ports compete, lower-priority ports are limited "
        "to this value. The highest-priority port can use up to "
        "max_outstanding.",
    )
    address_phase_cycles = Param.Cycles(
        1,
        "Minimum cycles between accepting consecutive requests "
        "(models AHB address phase occupancy)",
    )
    buffer_hit_cycles = Param.Cycles(
        1,
        "Latency in cycles for a read buffer hit "
        "(models bus transfer time without memory access)",
    )

    # Per-port configuration (VectorParam indexed by port ID).
    # If the vector has fewer entries than connected ports, the last
    # value is reused for remaining ports.
    port_priority = VectorParam.Unsigned(
        [0],
        "Per-port priority (higher value = higher priority). "
        "Highest-priority port bypasses max_per_port limit. "
        "E.g., [0, 1] = port 0 low priority, port 1 high priority.",
    )
    port_read_buffer_size = VectorParam.Unsigned(
        [0],
        "Per-port read buffer size in bytes (0 = disabled). "
        "Each port has its own read buffer modelling the flash "
        "controller's per-interface read register. Must be 0 or "
        "power of 2.  E.g., [8, 8] = both ports have 8-byte buffers.",
    )

    # Legacy single-value param kept for backward compatibility.
    # If port_read_buffer_size is not set, this value is used for all ports.
    read_buffer_size = Param.Unsigned(
        0,
        "Default read buffer size for all ports (overridden by "
        "port_read_buffer_size if set). Kept for backward compatibility.",
    )

    def controller(self):
        return self
