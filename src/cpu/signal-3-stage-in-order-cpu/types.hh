/*
 * Copyright (c) 2026 Zhantong Qiu, University of California, Davis
 * and Cornell University
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

#ifndef __CPU_SIGNAL_3_STAGE_IN_ORDER_CPU_TYPES_HH__
#define __CPU_SIGNAL_3_STAGE_IN_ORDER_CPU_TYPES_HH__

#include <cstdint>
#include <memory>
#include <optional>

#include "arch/generic/pcstate.hh"
#include "base/types.hh"
#include "cpu/static_inst.hh"

namespace gem5
{
namespace signal3
{

/**
 * A single 32-bit fetch word as it travels from the icache port into
 * the F stage's FIFO.  The F stage deals in fetch words, not
 * instructions — keeps F ISA-agnostic.
 *
 * `streamId` tags the AHB stream the request was issued on; responses
 * with a stale streamId (from before a redirect) are dropped at
 * IcachePort::recvTimingResp.
 */
struct FetchWord
{
    Addr addr = 0;
    uint32_t data = 0;
    uint32_t streamId = 0;

    bool
    operator==(const FetchWord &o) const
    {
        return addr == o.addr
            && data == o.data
            && streamId == o.streamId;
    }
    bool operator!=(const FetchWord &o) const { return !(*this == o); }
};

/**
 * A decoded instruction in the D->E pipeline register.  Mirrors
 * the predecessor's DecodedSlotEntry; pcStateAtDecode holds the
 * post-decode PC (with ISA-specific bits like Thumb mode preserved)
 * so Execute can restore it before invoking staticInst->execute().
 *
 * earlyResolved/resolvedNpc are populated in S5 by Decode's
 * E->D-flag-forwarding path; for S2/S3/S4 they default to {false, 0}.
 */
struct DecodedSlot
{
    Addr addr = 0;
    StaticInstPtr staticInst;
    std::shared_ptr<PCStateBase> pcStateAtDecode;
    bool earlyResolved = false;
    Addr resolvedNpc = 0;

    bool
    operator==(const DecodedSlot &o) const
    {
        // Identity-based: two slots are "the same" iff they represent
        // the same dynamic instruction.  Used by Signal's anti-cascade
        // guard.  StaticInstPtr identity (raw pointer) suffices.
        return addr == o.addr
            && staticInst.get() == o.staticInst.get()
            && earlyResolved == o.earlyResolved
            && resolvedNpc == o.resolvedNpc;
    }
    bool operator!=(const DecodedSlot &o) const { return !(*this == o); }
};

/**
 * Two 'sides' for the asynchronous response router.  Selected by the
 * port that called Pipeline::onAsyncResponse.
 */
enum class PortSide : uint8_t { Icache, Dcache };

} // namespace signal3
} // namespace gem5

#endif // __CPU_SIGNAL_3_STAGE_IN_ORDER_CPU_TYPES_HH__
