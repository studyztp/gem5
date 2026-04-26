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

#include "cpu/signal-3-stage-in-order-cpu/decode.hh"

#include <cstring>

#include "arch/generic/decoder.hh"
#include "arch/generic/pcstate.hh"
#include "cpu/signal-3-stage-in-order-cpu/exec_context.hh"
#include "cpu/signal-3-stage-in-order-cpu/fetch.hh"
#include "cpu/signal-3-stage-in-order-cpu/signal_cpu.hh"
#include "cpu/static_inst.hh"
#include "cpu/thread_context.hh"
#include "debug/Signal3CPUDecode.hh"
#include "sim/faults.hh"

namespace gem5
{
namespace signal3
{

Decode::Decode(SignalGraph &g, SignalCPU &cpu, Fetch &fetch)
    : Stage(g, "decode", /*stageId=*/1),
      dSlot(g, "dSlot", /*fromStage=*/this),
      d_pop_request(g, "d_pop_request_out", /*fromStage=*/this,
                    /*pulse=*/true),
      d_redirect_to_f(g, "d_redirect_to_f", /*fromStage=*/this,
                      /*pulse=*/true),
      _cpu(cpu),
      _fetch(fetch)
{
    // Re-fire when F provides a new word; back-pressure subscription
    // (E's e_accept_d) is wired by Pipeline once Execute exists.
    _fetch.f_word_out.subscribe(this);
}

void
Decode::resetTo(Addr entryPc)
{
    _nextInstrAddrToDeliver = entryPc;
    _haveLoadedWord = false;
    _lastFetchPc = 0;
    _decodedThisCycle = false;
    DPRINTF(Signal3CPUDecode, "resetTo entry=%#x\n", entryPc);
}

void
Decode::beginCycle()
{
    _decodedThisCycle = false;
    _slotDecisionMadeThisCycle = false;
}

void
Decode::applyRedirect(Addr target)
{
    DPRINTF(Signal3CPUDecode,
            "applyRedirect target=%#x (drop dSlot, reset decoder)\n",
            target);
    _nextInstrAddrToDeliver = target;
    _haveLoadedWord = false;
    _lastFetchPc = 0;
    // Drop the speculatively-latched dSlot — it's wrong-path.  Mark
    // consumed so a fresh decode can proceed once F refills the FIFO.
    dSlot.in().write(std::optional<DecodedSlot>{});
    _slotDecidedConsumed = true;
    _slotDecisionMadeThisCycle = true;
    // Reset the gem5 generic decoder.  It carries internal state
    // (mid-Thumb-2-decode flag, pending halfword) that, if left
    // behind, mis-classifies the post-redirect bytes as the second
    // halfword of a 32-bit Thumb-2 instruction.  Mirrors the
    // BaseSimpleCPU::checkForInterrupts path's `decoder->reset()`
    // call after fault invocation.
    ThreadContext *tc = _cpu.thread(0)->getTC();
    tc->getDecoderPtr()->reset();
    // Don't set _decodedThisCycle — D may legitimately decode the
    // first inst of the new stream this same cycle if F delivers it.
}

void
Decode::settle()
{
    // 0. React to E's redirect (if any) BEFORE slot management.
    //    The redirect invalidates dSlot and the decoder cache.
    if (_eRedirectInput && _eRedirectInput->valid()
            && _eRedirectInput->read().has_value()) {
        applyRedirect(*_eRedirectInput->read());
        // Fall through to refill from F's (post-redirect) FIFO.
    }

    // 0b. S5 same-cycle re-resolve:  if our pending dSlot.in() has
    //     a conditional branch that hasn't been early-resolved yet,
    //     and e_flagSetterNextPc just fired, attempt resolution.
    //     This handles the cascade where D decoded the branch in
    //     iter 1 (before E ran) and E pulses the forward signal in
    //     iter 1, triggering D's iter 2 to resolve.
    if (dSlot.in().valid() && dSlot.in().read().has_value()) {
        DecodedSlot pending = *dSlot.in().read();
        if (!pending.earlyResolved
                && pending.staticInst
                && pending.staticInst->isControl()
                && !pending.staticInst->isIndirectCtrl()) {
            if (tryEarlyResolveBranch(pending)) {
                dSlot.in().write(std::optional<DecodedSlot>{pending});
            }
        }
    }

    // ----------------------------------------------------------------
    // 1. Slot management.
    //
    // dSlot is a Latch: dSlot.out() shows the value latched at this
    // cycle's Edge (i.e. what D wrote last cycle); dSlot.in() is what
    // we will sample at the NEXT Edge.  If E was going to accept the
    // latched slot this cycle, write nullopt to mark it consumed; if
    // E is stalled, hold the value across the next Edge by re-asserting
    // it into in().
    // ----------------------------------------------------------------
    // The consume-vs-hold decision is made ONCE per cycle on the
    // first fire and persisted; later re-fires (e.g. E going Pending
    // mid-cycle) must not overwrite a freshly-decoded successor.
    if (!_slotDecisionMadeThisCycle) {
        const bool eAccepts = _eAcceptInput
                              && _eAcceptInput->valid()
                              && _eAcceptInput->read();
        const auto &latched = dSlot.out().valid()
                                  ? dSlot.out().read()
                                  : std::optional<DecodedSlot>{};
        if (latched.has_value()) {
            if (eAccepts) {
                dSlot.in().write(std::optional<DecodedSlot>{});
                _slotDecidedConsumed = true;
                DPRINTF(Signal3CPUDecode,
                        "dSlot consumed by E (addr=%#x)\n",
                        latched->addr);
            } else {
                dSlot.in().write(latched);
                _slotDecidedConsumed = false;
                DPRINTF(Signal3CPUDecode,
                        "dSlot stalls (E not accepting)\n");
            }
        } else {
            _slotDecidedConsumed = true;   // empty -> can refill
        }
        _slotDecisionMadeThisCycle = true;
    }

    if (!_slotDecidedConsumed)
        return;

    // Hard cap: at most one decode per cycle.  This matches the
    // Cortex-M4 D stage's single-issue throughput and prevents the
    // Settle loop from chaining multiple decodes within a single
    // cycle when (e.g.) a Thumb 16+16 word lands and the decoder
    // technically has bytes for two instructions.
    if (_decodedThisCycle)
        return;

    // ----------------------------------------------------------------
    // 2. Drive the decoder.  Mirrors the predecessor's stepOneInstr.
    // ----------------------------------------------------------------
    ThreadContext *tc = _cpu.thread(0)->getTC();
    InstDecoder *decoder = tc->getDecoderPtr();

    std::unique_ptr<PCStateBase> instrPc(tc->pcState().clone());
    instrPc->set(_nextInstrAddrToDeliver);

    if (!decoder->instReady()) {
        if (decoder->needMoreBytes() || !_haveLoadedWord) {
            // Need a fetch word from F.
            if (!_fetch.f_word_out.valid()
                    || !_fetch.f_word_out.read().has_value()) {
                DPRINTF(Signal3CPUDecode,
                        "no FIFO word for pc=%#x\n",
                        _nextInstrAddrToDeliver);
                return;
            }
            FetchWord w = *_fetch.f_word_out.read();
            // R6 word-level filter: skip wrong-path leftovers.
            if (w.addr + sizeof(uint32_t) <= _nextInstrAddrToDeliver) {
                DPRINTF(Signal3CPUDecode,
                        "skip wrong-path word %#x (below %#x); pop\n",
                        w.addr, _nextInstrAddrToDeliver);
                d_pop_request.write(true);
                return;   // re-fire when F's pop updates f_word_out
            }
            const size_t mb_size = decoder->moreBytesSize();
            DPRINTF(Signal3CPUDecode,
                    "feed addr=%#x data=%#08x to decoder (%lu bytes)\n",
                    w.addr, w.data, (unsigned long)mb_size);
            std::memcpy(decoder->moreBytesPtr(), &w.data,
                        std::min(mb_size, sizeof(uint32_t)));
            decoder->moreBytes(*instrPc, w.addr);
            _haveLoadedWord = true;
            _lastFetchPc = w.addr;
            // We've consumed this word's bytes — F should pop its
            // FIFO head this same Settle pass.  Pulse the request.
            d_pop_request.write(true);
        } else {
            DPRINTF(Signal3CPUDecode,
                    "reuse word (lastFetchPc=%#x) for pc=%#x\n",
                    _lastFetchPc, _nextInstrAddrToDeliver);
            decoder->moreBytes(*instrPc, _lastFetchPc);
        }
    }

    if (!decoder->instReady())
        return;

    StaticInstPtr staticInst = decoder->decode(*instrPc);
    if (!staticInst)
        return;

    DecodedSlot ds;
    ds.addr = _nextInstrAddrToDeliver;
    ds.staticInst = staticInst;
    ds.pcStateAtDecode.reset(instrPc->clone());

    DPRINTF(Signal3CPUDecode,
            "decoded inst @ pc=%#x size=%u isControl=%d isCondCtrl=%d "
            "isIndirectCtrl=%d\n",
            ds.addr, staticInst->size(),
            (int)staticInst->isControl(),
            (int)staticInst->isCondCtrl(),
            (int)staticInst->isIndirectCtrl());

    _nextInstrAddrToDeliver = ds.addr + staticInst->size();
    _decodedThisCycle = true;

    // S5: try to early-resolve a conditional branch using flags
    // forwarded from E (e_flagSetterNextPc must match this branch's
    // pc).  Updates ds.earlyResolved + ds.resolvedNpc and pulses
    // d_redirect_to_f if the branch is taken.
    tryEarlyResolveBranch(ds);

    dSlot.in().write(std::optional<DecodedSlot>{ds});
    // instsCommitted is bumped by Execute when an inst retires.  D
    // used it as a proxy in S2; with E in place that double-counts,
    // so we remove the bump here.
}

bool
Decode::tryEarlyResolveBranch(DecodedSlot &ds)
{
    if (!ds.staticInst->isControl())
        return false;
    // Direct conditional + unconditional branches resolve at D from
    // their immediate operand.  Indirect branches (BX, BLX-reg)
    // need the destination register's value, which D doesn't read,
    // so they fall through to E-resolution.
    if (ds.staticInst->isIndirectCtrl())
        return false;
    if (!_eFlagSetterInput
            || !_eFlagSetterInput->valid()
            || !_eFlagSetterInput->read().has_value()) {
        // For UNCONDITIONAL direct branches we can resolve without
        // any forwarded flags.  For CONDITIONAL ones we need the
        // forwarded flag pulse from E.
        if (ds.staticInst->isCondCtrl())
            return false;
        // Fall through: unconditional direct branch resolves with
        // current tc state (flags don't matter).
    } else if (ds.staticInst->isCondCtrl()) {
        // Verify the forwarded predecessor was the inst right before
        // this branch in PC order.  If not, the flags may be from
        // a stale producer and we must defer to E.
        const Addr expected = *_eFlagSetterInput->read();
        if (expected != ds.addr) {
            DPRINTF(Signal3CPUDecode,
                    "skip early-resolve: e_flagSetterNextPc=%#x "
                    "doesn't match branch pc=%#x\n",
                    expected, ds.addr);
            return false;
        }
    }

    // Speculatively run the branch's execute() against the current
    // thread context.  Save/restore tc.pcState() around the call.
    SimpleThread &thread = *_cpu.thread(0);
    std::unique_ptr<PCStateBase> savedPc(thread.pcState().clone());
    thread.pcState(*ds.pcStateAtDecode);
    ExecContext ctx(_cpu, thread);
    Fault fault = ds.staticInst->execute(&ctx, /*traceData=*/nullptr);
    panic_if(fault != NoFault,
             "Decode::tryEarlyResolveBranch: fault from execute()");
    ds.staticInst->advancePC(thread.getTC());
    const Addr resolvedNpc = thread.pcState().instAddr();
    // Restore tc.pcState — E will set it again from pcStateAtDecode
    // before commit (or use the fast path if we mark earlyResolved).
    thread.pcState(*savedPc);

    ds.earlyResolved = true;
    ds.resolvedNpc = resolvedNpc;

    const Addr fallThrough = ds.addr + ds.staticInst->size();
    if (resolvedNpc != fallThrough) {
        DPRINTF(Signal3CPUDecode,
                "early-resolve TAKEN pc=%#x -> %#x (forwarded)\n",
                ds.addr, resolvedNpc);
        d_redirect_to_f.write(std::optional<Addr>{resolvedNpc});
        // D's own internal stream also redirects: future decodes
        // start at the new target, not at the fall-through that
        // _nextInstrAddrToDeliver currently points to.  The decoder
        // cache must be invalidated since the target word's bytes
        // are different.
        _nextInstrAddrToDeliver = resolvedNpc;
        _haveLoadedWord = false;
        _lastFetchPc = 0;
    } else {
        DPRINTF(Signal3CPUDecode,
                "early-resolve NOT-TAKEN pc=%#x (fall-through)\n",
                ds.addr);
        // No redirect needed; the slot is marked earlyResolved with
        // resolvedNpc=fallThrough so E commits as a no-op.
    }
    return true;
}

} // namespace signal3
} // namespace gem5
