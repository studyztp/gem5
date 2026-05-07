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

    /* Size in BYTES of the architectural instruction this slot
     * represents.  For non-macro insts this equals
     * staticInst->size().  For macro-op micro-ops (LDM/STM/PUSH/POP
     * children) this equals the parent macro's size so E can compute
     * the architectural fall-through PC without depending on the
     * micro-op's _size being set (gem5's ARM decoder sets _size on
     * the macro and propagates via the PredMacroOp::size() override,
     * but the propagation isn't always observed in time for cached
     * micro-op pointers, so we carry the authoritative size here). */
    uint8_t instSize = 0;

    /* True iff this slot's staticInst is a micro-op that is the LAST
     * micro-op of a macro (or any non-micro inst).  Used by E to
     * decide when to emit redirect / flag-forward signals — only the
     * last micro-op architecturally commits the macro. */
    bool isLastInMacro = true;

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
 * NZCV flags forwarded from E to D in the same cycle E commits a
 * flag-updating instruction.  Lets D resolve a 16-bit T1 Bcond
 * combinationally against the just-committed flags instead of going
 * through E's full redirect bubble (saves the wrong-path Flash
 * serialization that motivated the +25 % calibration gap on the
 * forward / align / alternating benchmarks).
 *
 * Producer: E's commitOne() reads CPSR after the inst executes and
 * pulses the signal.  Consumer: D's tryDecodeOnlyResolveCondBranch
 * reads the forwarded flags only — no misc-reg fallback — because
 * StageOrder dispatches D (stageId=1) before E (stageId=2), so a
 * misc-reg read on D's first fire would always see pre-commit flags
 * in the very cycles where forwarding matters.
 */
struct ForwardedFlags
{
    uint8_t nz = 0;
    bool c = false;
    bool v = false;

    bool
    operator==(const ForwardedFlags &o) const
    {
        return nz == o.nz && c == o.c && v == o.v;
    }
    bool operator!=(const ForwardedFlags &o) const { return !(*this == o); }
};

/**
 * Two 'sides' for the asynchronous response router.  Selected by the
 * port that called Pipeline::onAsyncResponse.
 */
enum class PortSide : uint8_t { Icache, Dcache };

} // namespace signal3
} // namespace gem5

#endif // __CPU_SIGNAL_3_STAGE_IN_ORDER_CPU_TYPES_HH__
