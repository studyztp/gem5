/*
 * Copyright (c) 2026 University of California, Davis and Cornell University
 * All rights reserved
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

#ifndef __ARCH_ARM_M_FS_WORKLOAD_HH__
#define __ARCH_ARM_M_FS_WORKLOAD_HH__

/**
 * @file
 * M-profile (ARMv7-M / ARMv8-M) full-system workload.
 *
 * Loads a bare-metal firmware ELF into physical memory and boots via
 * MProfileReset (reads initial MSP and PC from the VTOR vector table).
 * No bootloader, no DTB, no GIC setup, no A-profile Reset fault.
 */

#include "arch/arm/remote_gdb.hh"
#include "params/ArmMFsWorkload.hh"
#include "sim/kernel_workload.hh"

namespace gem5
{

namespace ArmISA
{

class MFsWorkload : public KernelWorkload
{
  public:
    PARAMS(ArmMFsWorkload);

    MFsWorkload(const Params &p);

    /**
     * Load the firmware binary and perform M-profile reset.
     *
     * 1. KernelWorkload::initState() — load ELF into physProxy
     * 2. MProfileReset().invoke(tc)  — read MSP/PC from vector table
     * 3. tc->activate()              — start executing
     */
    void initState() override;

    void setSystem(System *sys) override;

    Addr
    getEntry() const override
    {
        if (kernelObj)
            return kernelObj->entryPoint();
        return 0;
    }

    /** M-profile is always 32-bit ARM (Thumb). */
    loader::Arch getArch() const override { return loader::Arm; }

    /** M-profile is little-endian. */
    ByteOrder byteOrder() const override { return ByteOrder::little; }

    Addr
    fixFuncEventAddr(Addr addr) const override
    {
        // Remove the low bit that Thumb symbols have set
        // but that aren't actually odd aligned.
        return addr & ~1;
    }
};

} // namespace ArmISA
} // namespace gem5

#endif // __ARCH_ARM_M_FS_WORKLOAD_HH__
