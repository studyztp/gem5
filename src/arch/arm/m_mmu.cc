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

#include "arch/arm/m_mmu.hh"

#include "arch/arm/m_faults.hh"
#include "arch/arm/m_interrupts.hh"
#include "arch/arm/page_size.hh"
#include "arch/arm/pcstate.hh"
#include "cpu/base.hh"
#include "cpu/thread_context.hh"
#include "sim/faults.hh"

namespace gem5
{

namespace ArmISA
{

// ---------------------------------------------------------------------------
// MTLB — pass-through TLB (identity translation, no protection)
// ---------------------------------------------------------------------------

Fault
MTLB::translateAtomic(const RequestPtr &req, ThreadContext *tc,
                      BaseMMU::Mode mode)
{
    Addr vaddr = req->getVaddr();

    // EXC_RETURN detection for POP {PC} / LDM {PC}.
    //
    // When an LDM/POP micro-op loads an EXC_RETURN value (0xFFFFFFF_)
    // into PC, the CPU tries to fetch the next instruction from that
    // address.  On real Cortex-M hardware, the bus matrix detects this
    // address range and triggers exception return instead of a memory
    // access.  In gem5, there's no memory at 0xFFFFFFF_ so the fetch
    // would panic.
    //
    // We detect this in the MMU translate path (before the fetch
    // reaches memory) and return an ArmMFault that performs the
    // exception return.  The fault's invoke() calls
    // mProfileExcReturn() which pops the exception frame.
    //
    // DDI0403E B1.5.8: Exception return occurs when PC is loaded
    // with a value where bits[31:4] are all 1s (EXC_RETURN prefix).
    // Valid range: 0xFFFFFFF0–0xFFFFFFFF.  The low 4 bits encode
    // which SP and mode to restore to.
    if (mode == BaseMMU::Execute &&
        (vaddr & 0xFFFFFFF0) == 0xFFFFFFF0) {
        // Exception return: deactivate via interrupt controller, then
        // unstack CPU state.  MProfileInterrupts::excReturn() handles both.
        // excReturn() calls mProfileExcReturnUnstack() which uses
        // pc.npc(retAddr).  In this MMU path, advancePC() is NOT called
        // (the fault path skips curStaticInst->advancePC()), so we must
        // manually advance the PC after excReturn returns.
        auto *mintr = dynamic_cast<MProfileInterrupts *>(
            tc->getCpuPtr()->getInterruptController(tc->threadId()));
        assert(mintr && "M-profile CPU must use MProfileInterrupts");
        mintr->excReturn(tc, (uint32_t)vaddr);
        // excReturn set _npc = retAddr.  Advance _pc = _npc now so
        // that the next fetch (after ReExec::invoke no-op) comes from retAddr,
        // not from 0xFFFFFFF8 again (which would re-trigger this path).
        auto &pc_base = tc->pcState();
        auto pc = pc_base.as<PCState>();
        pc.advance();  // _pc = _npc (retAddr)
        tc->pcState(pc);
        // Return a fault to skip fetchInstMem() and preExecute().
        // ReExec::invoke is a no-op; the CPU re-checks PC on the next tick.
        return std::make_shared<ReExec>();
    }

    // M-profile: VA == PA, no translation.
    // TODO: Add MPU permission checks here when the MPU is modelled.
    req->setPaddr(vaddr);
    return NoFault;
}

void
MTLB::translateTiming(const RequestPtr &req, ThreadContext *tc,
                      BaseMMU::Translation *translation,
                      BaseMMU::Mode mode)
{
    Addr vaddr = req->getVaddr();

    // EXC_RETURN detection — same logic as translateAtomic().
    // Without this, TimingSimpleCPU/MinorCPU panic trying to fetch
    // from 0xFFFFFFF_ (no memory at that address).
    //
    // Timing note: the detection itself is correctly placed (the real
    // bus matrix intercepts this address before it reaches memory, so
    // no icache latency should be charged).  However,
    // mProfileExcReturnUnstack() reads the 8-word exception frame via
    // physProxy (functional port) — zero memory latency.  On real
    // Cortex-M4 this unstacking takes ~12 cycles through the bus
    // matrix → SRAM.  A future fix should route these reads through
    // the timed memory system.  The same inaccuracy exists in
    // translateAtomic() and in ArmMFault::invoke() (frame push).
    if (mode == BaseMMU::Execute &&
        (vaddr & 0xFFFFFFF0) == 0xFFFFFFF0) {
        auto *mintr = dynamic_cast<MProfileInterrupts *>(
            tc->getCpuPtr()->getInterruptController(tc->threadId()));
        assert(mintr && "M-profile CPU must use MProfileInterrupts");
        mintr->excReturn(tc, (uint32_t)vaddr);
        auto &pc_base = tc->pcState();
        auto pc = pc_base.as<PCState>();
        pc.advance();
        tc->pcState(pc);
        translation->finish(std::make_shared<ReExec>(), req, tc, mode);
        return;
    }

    // Pass-through: complete immediately with identity translation.
    req->setPaddr(vaddr);
    translation->finish(NoFault, req, tc, mode);
}

Fault
MTLB::translateFunctional(const RequestPtr &req, ThreadContext *tc,
                          BaseMMU::Mode mode)
{
    req->setPaddr(req->getVaddr());
    return NoFault;
}

Fault
MTLB::finalizePhysical(const RequestPtr &req, ThreadContext *tc,
                       BaseMMU::Mode mode) const
{
    return NoFault;
}

// ---------------------------------------------------------------------------
// MMMU — pass-through MMU
// ---------------------------------------------------------------------------

TranslationGenPtr
MMMU::translateFunctional(Addr start, Addr size, ThreadContext *tc,
                          Mode mode, Request::Flags flags)
{
    // Use the standard MMUTranslationGen with the ARM page size.
    // Since MTLB always returns VA == PA, the generator will produce
    // identity-mapped ranges.
    return TranslationGenPtr(new MMUTranslationGen(
            PageBytes, start, size, tc, this, mode, flags));
}

} // namespace ArmISA
} // namespace gem5
