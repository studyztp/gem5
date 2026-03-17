from m5.objects.ArmSystem import ArmRelease
from m5.objects.System import System
from m5.params import *


class ArmMRelease(ArmRelease):
    """ArmRelease pre-configured for M-profile (Cortex-M4)."""

    extensions = ["M_PROFILE"]


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
        ArmMRelease(),
        "Arm M-profile Release (must include M_PROFILE extension)",
    )
