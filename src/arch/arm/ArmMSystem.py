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

from m5.objects.ArmSemihosting import ArmSemihosting
from m5.objects.ArmSystem import ArmRelease
from m5.objects.System import System
from m5.params import *

# ====================================================================
# M-profile release classes
# ====================================================================
#
# Each class pre-configures ArmRelease.extensions for a specific
# Cortex-M architecture variant.  The inheritance chain mirrors the
# ARM architecture hierarchy — each level adds the extensions
# introduced by that architecture version:
#
#   ArmMRelease          (M_PROFILE)
#     └── CortexM0       (+ ARMV6M)
#           └── CortexM3 (+ ARMV7M)
#                 └── CortexM4 (+ ARMV7EM, DSP, FPU_SP)
#                       └── CortexM7 (+ FPU_DP)
#
# The extensions list is evaluated once at class definition time
# into a flat list — no runtime hierarchy traversal.
#
# The decoder uses has(extension) to gate instructions per variant:
#   has(M_PROFILE)          -> M-profile MRS/MSR/CPS/BX, block SMC/HVC
#   has(M_PROFILE_ARMV6M)   -> base Thumb subset
#   has(M_PROFILE_ARMV7M)   -> full Thumb-2, BASEPRI, FAULTMASK, IT
#   has(M_PROFILE_ARMV7EM)  -> DSP instruction base encoding space
#   has(M_PROFILE_DSP)      -> DSP/SIMD packed arithmetic
#   has(M_PROFILE_FPU_SP)   -> single-precision FP instructions
#   has(M_PROFILE_FPU_DP)   -> double-precision FP instructions
#
# References:
#   DDI0419  — ARMv6-M Architecture Reference Manual
#   DDI0403E — ARMv7-M Architecture Reference Manual
#   DDI0489  — Cortex-M7 Technical Reference Manual


class ArmMRelease(ArmRelease):
    """Base M-profile release.  All M-profile variants include M_PROFILE."""

    extensions = ["M_PROFILE"]


class ArmMReleaseCortexM0(ArmMRelease):
    """
    Cortex-M0 / M0+ / M1 — ARMv6-M.

    Minimal Thumb subset: 16-bit Thumb instructions plus only
    BL, MRS, MSR, DMB, DSB, ISB from Thumb-2.
    No BASEPRI, no FAULTMASK, no IT, no CBZ/CBNZ, no HW divide,
    no DSP, no FPU.

    Reference: DDI0419 (ARMv6-M Architecture Reference Manual)
    """

    extensions = ArmMRelease.extensions + ["M_PROFILE_ARMV6M"]


class ArmMReleaseCortexM3(ArmMReleaseCortexM0):
    """
    Cortex-M3 — ARMv7-M.

    Full Thumb + Thumb-2 instruction set.  Adds HW divide
    (SDIV/UDIV), bit field ops, saturation, exclusive access,
    IT blocks, CBZ/CBNZ, BASEPRI, FAULTMASK.  No DSP or FPU.

    Reference: DDI0403E (ARMv7-M Architecture Reference Manual)
    """

    extensions = ArmMReleaseCortexM0.extensions + ["M_PROFILE_ARMV7M"]


class ArmMReleaseCortexM4(ArmMReleaseCortexM3):
    """
    Cortex-M4 — ARMv7E-M with DSP + single-precision FPU.

    Adds DSP/SIMD packed arithmetic and optional VFPv4-SP FPU.
    This is the default ArmMRelease (primary implementation target).

    Reference: DDI0403E appendix (DSP extension),
               DDI0439 (Cortex-M4 Technical Reference Manual)
    """

    extensions = ArmMReleaseCortexM3.extensions + [
        "M_PROFILE_ARMV7EM",
        "M_PROFILE_DSP",
        "M_PROFILE_FPU_SP",
    ]


class ArmMReleaseCortexM4NoFPU(ArmMReleaseCortexM3):
    """
    Cortex-M4 without FPU — ARMv7E-M with DSP only.

    Some M4 variants (e.g., STM32F401) have DSP but no FPU.
    """

    extensions = ArmMReleaseCortexM3.extensions + [
        "M_PROFILE_ARMV7EM",
        "M_PROFILE_DSP",
    ]


class ArmMReleaseCortexM7(ArmMReleaseCortexM4):
    """
    Cortex-M7 — ARMv7E-M with DSP + double-precision FPU.

    Adds optional VFPv5 double-precision FPU and optional
    I-cache/D-cache (cache modelled by cache hierarchy, not ISA).

    Reference: DDI0489 (Cortex-M7 Technical Reference Manual)
    """

    extensions = ArmMReleaseCortexM4.extensions + ["M_PROFILE_FPU_DP"]


class ArmMSystem(System):
    """
    System object for ARM M-profile (Cortex-M) simulation.

    Inherits from System (not ArmSystem) because M-profile has no
    exception levels, GIC, Generic Timer, SVE/SME, or AArch64 state.
    """

    type = "ArmMSystem"
    cxx_header = "arch/arm/m_system.hh"
    cxx_class = "gem5::ArmMSystem"

    release = Param.ArmRelease(
        ArmMReleaseCortexM4(),
        "Arm M-profile Release (default: Cortex-M4 with DSP + FPU). "
        "Use ArmMReleaseCortexM0/M3/M7 for other variants.",
    )

    semihosting = Param.ArmSemihosting(
        NULL,
        "Enable ARM semihosting support. M-profile uses BKPT #0xAB "
        "as the trigger instruction (per ARM semihosting spec). "
        "Set to ArmSemihosting() to enable.",
    )

    # NOTE: The SCS device is owned by ArmMPlatform (not ArmMSystem).
    # MProfileSCS::init() registers itself with ArmMSystem via setSCS()
    # in C++.  No SCS param here to avoid SimObject hierarchy cycles.
