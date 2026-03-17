/*
 * Copyright (c) 2026 The gem5 Contributors
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

#include "arch/arm/page_size.hh"
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
    // M-profile: VA == PA, no translation.
    // TODO: Add MPU permission checks here when the MPU is modelled.
    req->setPaddr(req->getVaddr());
    return NoFault;
}

void
MTLB::translateTiming(const RequestPtr &req, ThreadContext *tc,
                      BaseMMU::Translation *translation,
                      BaseMMU::Mode mode)
{
    // Pass-through: complete immediately with identity translation.
    req->setPaddr(req->getVaddr());
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
