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

    latency = Param.Latency("30ns", "Request to response latency")
    latency_var = Param.Latency("0ns", "Request to response latency variance")

    # Shared pipeline params
    address_phase_latency = Param.Latency(
        "0ns",
        "Latency for the address phase before a request can be processed "
        "(models AHB address phase occupancy)",
    )
    buffer_hit_latency = Param.Latency(
        "0ns",
        "Latency for a read buffer hit "
        "(models bus transfer time without memory access)",
    )
    memory_read_request_size = Param.Unsigned(
        8,
        "The read request size in bytes to the memory. "
        "Per-port read_buffer_size must not exceed this value.",
    )

    # Per-port configuration (VectorParam indexed by port ID).
    # If the vector has fewer entries than connected ports, the last
    # value is reused for remaining ports.
    port_priority = VectorParam.Unsigned(
        [0],
        "Per-port priority (higher value = higher priority). "
        "E.g., [0, 1] = port 0 low priority, port 1 high priority.",
    )
    port_read_buffer_size = VectorParam.Unsigned(
        [0],
        "Per-port read buffer size in bytes (0 = disabled). "
        "Must be 0 or power of 2, and at most memory_read_request_size. "
        "E.g., [8, 8] = both ports have 8-byte buffers.",
    )
    port_arrive_buffer_size = VectorParam.Unsigned(
        [0],
        "Per-port arrive buffer size limit (0 = unlimited). "
        "Limits how many requests can queue before address phase. "
        "E.g., [2, 2] = each port buffers up to 2 incoming requests.",
    )
    port_ready_to_fire_buffer_size = VectorParam.Unsigned(
        [0],
        "Per-port ready-to-fire buffer size limit (0 = unlimited). "
        "Limits how many requests can wait for flash arbitration. "
        "E.g., [1, 1] = each port has 1 slot for flash arbitration.",
    )

    # Default single-value params for all ports.
    # Overridden by the per-port VectorParam if set.
    read_buffer_size = Param.Unsigned(
        0,
        "Default read buffer size for all ports (overridden by "
        "port_read_buffer_size if set).",
    )
    arrive_buffer_size = Param.Unsigned(
        0,
        "Default arrive buffer size limit for all ports (0 = unlimited). "
        "Overridden by port_arrive_buffer_size if set.",
    )
    ready_to_fire_buffer_size = Param.Unsigned(
        0,
        "Default ready-to-fire buffer size limit for all ports "
        "(0 = unlimited). Overridden by port_ready_to_fire_buffer_size "
        "if set.",
    )

    def controller(self):
        return self
