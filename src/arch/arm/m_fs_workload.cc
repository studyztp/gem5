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

#include "arch/arm/m_fs_workload.hh"

#include "arch/arm/m_faults.hh"
#include "arch/arm/regs/misc.hh"
#include "cpu/thread_context.hh"
#include "sim/system.hh"

namespace gem5
{

namespace ArmISA
{

MFsWorkload::MFsWorkload(const Params &p)
    : KernelWorkload(p)
{
}

void
MFsWorkload::initState()
{
    // Step 1: Load the firmware ELF binary into physical memory.
    KernelWorkload::initState();

    // Step 2: Set VTOR to point to the firmware's vector table.
    //
    // On real Cortex-M hardware, VTOR resets to 0x00000000 and
    // a boot alias mirrors flash there.  In gem5, we don't always
    // have a boot alias, so we set VTOR to the ELF's entry point
    // region (the start of the .isr_vector section = start of flash).
    //
    // The ELF entry point is the Reset_Handler address, which is
    // stored at VTOR+4.  The vector table starts at the beginning
    // of the first loadable segment (typically the flash base).
    //
    // KernelWorkload stores the loaded object.  We use its text
    // base as the vector table address (the .isr_vector section
    // is the first thing in flash, before .text).
    // Step 2: Find the vector table address from the ELF image.
    //
    // The vector table (.isr_vector) is the first thing in the ELF,
    // placed at the start of flash by the linker script.
    // buildImage().minAddr() gives the lowest load address = VTOR.
    //
    // On real hardware, VTOR resets to 0 and a boot alias maps flash
    // there.  In gem5, we pass the actual flash address to
    // MProfileReset so it reads the vector table correctly.
    Addr vtor_addr = kernelObj->buildImage().minAddr();

    // Step 3: Perform M-profile reset for each thread context.
    // MProfileReset(vtor_addr) sets VTOR after clearing arch regs,
    // then reads initial SP from VTOR+0 and PC from VTOR+4.
    for (auto *tc : system->threads) {
        MProfileReset(vtor_addr).invoke(tc);
        tc->activate();
    }
}

void
MFsWorkload::setSystem(System *sys)
{
    KernelWorkload::setSystem(sys);
    gdb = BaseRemoteGDB::build<RemoteGDB>(
            params().remote_gdb_port, system);
}

} // namespace ArmISA
} // namespace gem5
