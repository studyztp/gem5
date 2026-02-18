/*
* Copyright (c) 2026 The Regents of the University of California.
* All rights reserved.
*
* Redistribution and use in source and binary forms, with or without
* modification, are permitted provided that the following conditions are
* met: redistributions of source code must retain the above copyright
* notice, this list of conditions and the following disclaimer;
* redistributions in binary form must reproduce the above copyright
* notice, this list of conditions and the following disclaimer in the
* documentation and/or other materials provided with the distribution;
* neither the name of the copyright holders nor the names of its
* contributors may be used to endorse or promote products derived from
* this software without specific prior written permission.
*
* THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
* "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
* LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
* A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
* OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
* SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
* LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
* DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
* THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
* (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
* OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/


#ifndef __ARCH_ARM_MPROFILE_INTERRUPT_HH__
#define __ARCH_ARM_MPROFILE_INTERRUPT_HH__

#include "arch/arm/mprofile/faults.hh"
#include "arch/arm/regs/misc.hh"
#include "arch/arm/utility.hh"
#include "arch/generic/interrupts.hh"
#include "cpu/thread_context.hh"
#include "debug/MProfileInterrupt.hh"
#include "enums/ArmExtension.hh"
#include "params/ArmMProfileInterrupts.hh"

namespace gem5
{

namespace ArmISA
{

// 16 System Exceptions + 240 IRQs
static const int NumMProfileInterruptTypes = 256; 

enum MProfileInterruptTypes
{
    RESET,
    NMI,
    HARD_FAULT,
    MEMORY_MANAGEMENT_FAULT,
    BUS_FAULT,
    USAGE_FAULT,
    SVC_CALL,
    DEBUG_MONITOR,
    Reserved,
    PENDING_SV,
    SYSTICK,
    IRQS,
    UNKNOWN
};

class ArmMProfileInterrupts : public BaseInterrupts
{
private:
    bool interrupts[NumMProfileInterruptTypes];
    // Use this to track if there are any interrupts pending.
    int intStatus = 0; // default to no interrupts pending

public:
    using Params = ArmMProfileInterruptsParams;

    ArmMProfileInterrupts(const Params &p);

    void
    post(int int_num, int index) override
    {
        DPRINTF(MProfileInterrupt, "Interrupt %d:%d posted\n", int_num, index);

        if (int_num < 0 || int_num >= NumMProfileInterruptTypes)
            panic("int_num out of bounds\n");

        if (index != 0)
            panic("No support for other interrupt indexes\n");

        interrupts[int_num] = true;
        intStatus ++;
    }

    void
    clear(int int_num, int index) override
    {
        DPRINTF(Interrupt, "Interrupt %d:%d cleared\n", int_num, index);

        if (int_num < 0 || int_num >= NumInterruptTypes)
            panic("int_num out of bounds\n");

        if (index != 0)
            panic("No support for other interrupt indexes\n");

        interrupts[int_num] = false;
        intStatus --;
    }

    void
    clearAll() override
    {
        DPRINTF(Interrupt, "Interrupts all cleared\n");
        intStatus = 0;
        memset(interrupts, 0, sizeof(interrupts));
    }

    bool takeInt(MProfileInterruptTypes int_type) const;

    bool
    checkInterrupts() const override
    {
        // if there are any interrupts pending, return true
        return intStatus > 0;
    }

    /**
    * This function is used to check if a wfi operation should sleep. If there
    * is an interrupt pending, even if it's masked, wfi doesn't sleep.
    * @return any interrupts pending
    */
    bool
    checkWfiWake(HCR hcr, CPSR cpsr, SCR scr) const
    {
        uint64_t masked_int_status;
        bool     virt_wake;

        masked_int_status = intStatus & ~((1 << INT_VIRT_IRQ) |
                                        (1 << INT_VIRT_FIQ));
        virt_wake  = (hcr.vi || interrupts[INT_VIRT_IRQ]) && hcr.imo;
        virt_wake |= (hcr.vf || interrupts[INT_VIRT_FIQ]) && hcr.fmo;
        virt_wake |=  hcr.va                              && hcr.amo;
        virt_wake &= currEL(cpsr) < EL2 && EL2Enabled(tc);
        return masked_int_status || virt_wake;
    }

    uint32_t
    getISR(HCR hcr, CPSR cpsr, SCR scr)
    {
        bool use_hcr_mux = currEL(cpsr) < EL2 && EL2Enabled(tc);
        ISR isr = 0;

        isr.i = (use_hcr_mux & hcr.imo) ? (interrupts[INT_VIRT_IRQ] || hcr.vi)
                                        :  interrupts[INT_IRQ];
        isr.f = (use_hcr_mux & hcr.fmo) ? (interrupts[INT_VIRT_FIQ] || hcr.vf)
                                        :  interrupts[INT_FIQ];
        isr.a = (use_hcr_mux & hcr.amo) ?  hcr.va : interrupts[INT_ABT];
        return isr;
    }

    /**
    * Check the state of a particular interrupt, ignoring CPSR masks.
    *
    * This method is primarily used when running the target CPU in a
    * hardware VM (e.g., KVM) to check if interrupts should be
    * delivered upon guest entry.
    *
    * @param interrupt Interrupt type to check the state of.
    * @return true if the interrupt is asserted, false otherwise.
    */
    bool
    checkRaw(InterruptTypes interrupt) const
    {
        if (interrupt >= NumInterruptTypes)
            panic("Interrupt number out of range.\n");

        return interrupts[interrupt];
    }

    Fault
    getInterrupt() override
    {
        assert(checkInterrupts());

        HCR  hcr  = tc->readMiscReg(MISCREG_HCR_EL2);

        if (interrupts[INT_IRQ] && takeInt(INT_IRQ))
            return std::make_shared<Interrupt>();
        if ((interrupts[INT_VIRT_IRQ] || hcr.vi) &&
            takeVirtualInt(INT_VIRT_IRQ))
            return std::make_shared<VirtualInterrupt>();
        if (interrupts[INT_FIQ] && takeInt(INT_FIQ))
            return std::make_shared<FastInterrupt>();
        if ((interrupts[INT_VIRT_FIQ] || hcr.vf) &&
            takeVirtualInt(INT_VIRT_FIQ))
            return std::make_shared<VirtualFastInterrupt>();
        if (interrupts[INT_ABT] && takeInt(INT_ABT))
            return std::make_shared<SystemError>();
        if (hcr.va && takeVirtualInt(INT_VIRT_ABT))
            return std::make_shared<VirtualDataAbort>(
                0, DomainType::NoAccess, false,
                ArmFault::AsynchronousExternalAbort);
        if (interrupts[INT_RST])
            return std::make_shared<Reset>();
        if (interrupts[INT_SEV])
            return std::make_shared<ArmSev>();

        panic("intStatus and interrupts not in sync\n");
    }

    void updateIntrInfo() override {} // nothing to do

    void
    serialize(CheckpointOut &cp) const override
    {
        SERIALIZE_ARRAY(interrupts, NumInterruptTypes);
        SERIALIZE_SCALAR(intStatus);
    }

    void
    unserialize(CheckpointIn &cp) override
    {
        UNSERIALIZE_ARRAY(interrupts, NumInterruptTypes);
        UNSERIALIZE_SCALAR(intStatus);
    }
};

} // namespace ARM_ISA
} // namespace gem5

#endif // __ARCH_ARM_MPROFILE_INTERRUPT_HH__
