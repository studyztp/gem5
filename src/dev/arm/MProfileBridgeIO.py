# Copyright (c) 2026 University of California, Davis and Cornell University
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

from m5.objects.Device import BasicPioDevice
from m5.objects.MProfileSCS import MProfileSCS
from m5.params import *
from m5.util.pybind import PyBindMethod


class MProfileBridgeIO(BasicPioDevice):
    """
    Arm-M-profile bridge I/O device.

    Memory-mapped device that lets a gem5 Python config script feed
    input data to simulated firmware, observe output data the firmware
    produces, and raise/clear an external IRQ on the M-profile NVIC
    (MProfileSCS).

    Modeled after the generic BridgeIODevice from gem5 commit 0d8839ea,
    but the interrupt path is replaced with a typed SCS pointer + IRQ
    index because M-profile uses an NVIC instead of a GIC.
    """

    type = "MProfileBridgeIO"
    cxx_class = "gem5::MProfileBridgeIO"
    cxx_header = "dev/arm/m_profile_bridge_io.hh"

    # Total MMIO window.  Must be >= 6*4 + input_data_buffer_size +
    # output_data_buffer_size.  Default 4 KiB matches the typical
    # peripheral page on M-profile devices and leaves headroom over
    # the default 1 KiB + 1 KiB buffers.
    pio_size = Param.Addr(0x1000, "Size of MMIO window in bytes")

    # The M-profile NVIC this bridge raises IRQs through.  Typed as
    # MProfileSCS (rather than a generic SimObject) so misconfigurations
    # are caught at config-time, not via a C++ dynamic_cast at runtime.
    scs = Param.MProfileSCS("M-profile SCS the bridge raises IRQs on")

    # 0-based external IRQ index, i.e. the NVIC IRQn that firmware
    # would use with NVIC->ISER[N/32] |= (1 << (N%32)).  Translated to
    # exception number = irq_num + 16 inside the SCS.  Bounds-checked
    # against scs.num_irqs in MProfileBridgeIO::init().
    irq_num = Param.UInt32("External IRQ index (0..scs.num_irqs - 1)")

    # Buffer sizes.  Defaults match the reference BridgeIODevice so
    # Python harnesses written for that device port over unchanged.
    input_data_buffer_size = Param.Int(
        1024, "Input buffer size in bytes (firmware reads, Python writes)"
    )
    output_data_buffer_size = Param.Int(
        1024, "Output buffer size in bytes (firmware writes, Python reads)"
    )

    # Initial state of the 'go' gate (register[0]).  When False, the
    # device refuses updateInputData() and raiseInterrupt() so a
    # harness can stage state before turning the bridge on.
    go = Param.Bool(True, "Initial value of register[0] (the 'go' gate)")

    # Python-callable entry points.  Names and surface mirror the
    # reference BridgeIODevice exactly so an existing Python harness
    # can be reused with a single class-name swap.
    cxx_exports = [
        PyBindMethod("updateDone"),
        PyBindMethod("raiseInterrupt"),
        PyBindMethod("updateInputData"),
        PyBindMethod("clearInterrupt"),
        PyBindMethod("ifDone"),
        PyBindMethod("getOutputData"),
        PyBindMethod("getOutputDataSize"),
    ]
