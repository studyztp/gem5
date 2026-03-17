from m5.objects.BaseMMU import BaseMMU
from m5.objects.BaseTLB import BaseTLB
from m5.params import *


class ArmMTLB(BaseTLB):
    """
    Pass-through TLB for M-profile (no address translation).

    M-profile has no MMU or TLB — all addresses are physical.  This
    class satisfies the BaseTLB interface by returning VA == PA for
    every translation request.

    TODO: When the optional MPU (Memory Protection Unit) is modelled,
    access-permission checks can be added here.
    """

    type = "ArmMTLB"
    cxx_header = "arch/arm/m_mmu.hh"
    cxx_class = "gem5::ArmISA::MTLB"

    entry_type = "unified"


class ArmMMMU(BaseMMU):
    """
    Pass-through MMU for M-profile (all addresses are physical).

    Delegates to ArmMTLB instances which perform identity translation.
    """

    type = "ArmMMMU"
    cxx_header = "arch/arm/m_mmu.hh"
    cxx_class = "gem5::ArmISA::MMMU"

    itb = ArmMTLB()
    dtb = ArmMTLB()
