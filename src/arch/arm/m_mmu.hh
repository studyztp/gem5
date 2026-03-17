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

#ifndef __ARCH_ARM_M_MMU_HH__
#define __ARCH_ARM_M_MMU_HH__

/**
 * @file
 * Pass-through MMU and TLB for M-profile (no address translation).
 *
 * M-profile has no MMU — all addresses are physical.  These classes
 * satisfy the BaseMMU/BaseTLB interface that gem5 CPU models require
 * by performing identity translation (VA == PA, always NoFault).
 *
 * TODO: When the optional MPU (Memory Protection Unit) is modelled,
 * permission checks can be added to MTLB::translateAtomic() etc.
 */

#include "arch/generic/mmu.hh"
#include "arch/generic/tlb.hh"
#include "params/ArmMMMU.hh"
#include "params/ArmMTLB.hh"

namespace gem5
{

namespace ArmISA
{

/**
 * Pass-through TLB — always returns NoFault, sets paddr = vaddr.
 */
class MTLB : public BaseTLB
{
  public:
    PARAMS(ArmMTLB);
    MTLB(const Params &p) : BaseTLB(p) {}

    void demapPage(Addr vaddr, uint64_t asn) override {}

    Fault translateAtomic(const RequestPtr &req, ThreadContext *tc,
                          BaseMMU::Mode mode) override;

    void translateTiming(const RequestPtr &req, ThreadContext *tc,
                         BaseMMU::Translation *translation,
                         BaseMMU::Mode mode) override;

    Fault translateFunctional(const RequestPtr &req, ThreadContext *tc,
                              BaseMMU::Mode mode) override;

    Fault finalizePhysical(const RequestPtr &req, ThreadContext *tc,
                           BaseMMU::Mode mode) const override;

    void flushAll() override {}
    void takeOverFrom(BaseTLB *otlb) override {}
};

/**
 * Pass-through MMU — delegates to MTLB for identity translation.
 */
class MMMU : public BaseMMU
{
  public:
    PARAMS(ArmMMMU);
    MMMU(const Params &p) : BaseMMU(p) {}

    TranslationGenPtr translateFunctional(
            Addr start, Addr size, ThreadContext *tc,
            Mode mode, Request::Flags flags) override;
};

} // namespace ArmISA
} // namespace gem5

#endif // __ARCH_ARM_M_MMU_HH__
