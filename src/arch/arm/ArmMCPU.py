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

from m5.objects.ArmMDecoder import ArmMDecoder
from m5.objects.ArmMInterrupts import MProfileInterrupts
from m5.objects.ArmMISA import ArmMISA
from m5.objects.ArmMMMU import ArmMMMU
from m5.objects.BaseAtomicSimpleCPU import BaseAtomicSimpleCPU
from m5.objects.BaseMinorCPU import BaseMinorCPU
from m5.objects.BaseTimingSimpleCPU import BaseTimingSimpleCPU
from m5.objects.Simple3CycleCPU import Simple3CycleCPU


class ArmMCPU:
    """
    M-profile CPU mixin.

    Binds ISA-independent CPU models to M-profile architecture components.
    Mirrors ArmCPU (A-profile) but uses M-profile ISA, MMU, and interrupts.
    The Thumb decoder is shared with A-profile (M-profile is Thumb-only).
    """

    ArchDecoder = ArmMDecoder
    ArchMMU = ArmMMMU
    ArchInterrupts = MProfileInterrupts
    ArchISA = ArmMISA


class ArmMAtomicSimpleCPU(BaseAtomicSimpleCPU, ArmMCPU):
    mmu = ArmMMMU()


class ArmMTimingSimpleCPU(BaseTimingSimpleCPU, ArmMCPU):
    mmu = ArmMMMU()


class ArmMMinorCPU(BaseMinorCPU, ArmMCPU):
    mmu = ArmMMMU()


class ArmMSimple3CycleCPU(Simple3CycleCPU, ArmMCPU):
    mmu = ArmMMMU()
