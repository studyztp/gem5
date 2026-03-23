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
from m5.params import *


class MProfileSCS(BasicPioDevice):
    """
    M-profile System Control Space (SCS) device.

    Models the 4KB memory-mapped region at 0xE000E000 containing:
      - System Control Block (SCB): bridges to ISA misc regs (VTOR, AIRCR, etc.)
      - Nested Vectored Interrupt Controller (NVIC): IRQ enable/pending/priority
      - SysTick timer: 24-bit countdown with interrupt generation

    On real Cortex-M hardware these are one tightly-coupled unit.
    One BasicPioDevice with internal address dispatch is the cleanest mapping.

    The parameters below are the knobs that differ across M-profile variants.
    Each one corresponds to a real hardware difference between Cortex-M0/M0+
    and Cortex-M3/M4/M7, documented inline so that future users know which
    values to set for their target variant.
    """

    type = "MProfileSCS"
    cxx_class = "gem5::MProfileSCS"
    cxx_header = "dev/arm/m_profile_scs.hh"

    # SCS base address per ARMv7-M architecture (fixed, not relocatable)
    pio_addr = 0xE000E000

    # --- Variant-configurable knobs ---

    # Number of external IRQ lines.  M0/M0+ supports up to 32;
    # M3/M4/M7 supports up to 240.
    # The Armv7-M profile supports two system-level interrupts, and up to 496
    # external interrupts.
    # The NVIC register space
    # (ISER/ICER/ISPR/ICPR/IABR/IPR) is sized to this value.
    num_irqs = Param.UInt32("Number of external IRQs supported (max 496)")

    # Number of implemented priority bits per IRQ.
    # M0/M0+ implements 2 bits (4 levels); M3 typically 3 (8 levels);
    # M4 typically 4 (16 levels); M7 up to 8 (256 levels).
    # Unimplemented low bits read-as-zero, writes ignored.
    # This directly affects the priority mask applied to IPR writes.
    priority_bits = Param.UInt8(
        "Number of implemented priority bits (2=M0, 3=M3, 4=M4, 8=max)"
    )

    # SysTick is mandatory on M3/M4/M7/M33/M55/M85 but optional on M0/M0+.
    # When False, SysTick register accesses return 0 / are ignored,
    # and no SysTick exception (exc 15) will ever be pended.
    # TODO: capping at 1 for now.
    num_systick = Param.UInt8(
        "The number of SysTick. (0=M0/M0+, 1=M3/M4/M7, 2=M33/M55/M85/max)"
    )

    # BASEPRI and FAULTMASK are available on M3/M4/M7 but absent on
    # M0/M0+/M23, which only has PRIMASK.  When False, executionPriority()
    # only checks PRIMASK, matching M0 hardware behavior.  When True,
    # executionPriority() also consults BASEPRI and FAULTMASK for
    # priority-based masking (needed for correct FreeRTOS critical
    # sections on M3/M4/M7).
    has_basepri = Param.Bool(
        "Whether BASEPRI/FAULTMASK exist (False for M0/M0+/M23)"
    )

    # SysTick CALIB register value.  Implementation-defined; encodes
    # the reload value for a 10ms tick at the reference clock frequency.
    # Bit[31] (NOREF) = 1 means no external reference clock.
    # Bit[30] (SKEW) = 1 means calibration value is not exactly 10ms.
    # Bits[23:0] = TENMS reload value (0 = calibration not known).
    # TODO: currently, I'm only allowing NOREF=1 and SKEW=0.
    systick_calib = Param.UInt32(
        "SysTick CALIB register value (implementation-defined)"
    )
