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
#include <sstream>

#include "arch/generic/decoder.hh"
#include "arch/generic/pcstate.hh"
#include "cpu/signal-3-stage-in-order-cpu/exec_context.hh"
#include "cpu/signal-3-stage-in-order-cpu/fetch.hh"
#include "cpu/signal-3-stage-in-order-cpu/pipeline.hh"
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
    _curMacro = nullptr;
    _microPC = 0;
    _macroPC.reset();
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
    // Drop any in-flight macro-op expansion — wrong-path.
    _curMacro = nullptr;
    _microPC = 0;
    _macroPC.reset();
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
    // 1.5  Macro-op micro-op streaming (Minor-style).
    //
    // ARM Thumb-2 LDM/STM/PUSH/POP and predicated multi-reg ops are
    // PredMacroOp StaticInsts whose execute() panics — they MUST be
    // expanded into micro-ops via fetchMicroop().  D iterates the
    // macro at exactly one micro-op per cycle (single-issue),
    // emitting each as a fresh DecodedSlot whose `staticInst` is a
    // micro-op.  This matches Cortex-M4 TRM Table 3-1's "1+N" cycle
    // count for LDM/STM/PUSH/POP via the F→D→E pipeline overlap.
    //
    // The fresh-decode path below sets `_curMacro` + `_macroPC`
    // when it sees a macro-op and emits the first micro-op there.
    // This block handles cycles 2..N of the expansion: emit the
    // next micro-op from the cached macro and advance `_macroPC`.
    // When the just-emitted micro-op is the last, clear macro state.
    // ----------------------------------------------------------------
    if (_curMacro) {
        const MicroPC upc = _macroPC->microPC();
        StaticInstPtr microInst = _curMacro->fetchMicroop(upc);

        DecodedSlot ds;
        ds.addr = _macroPC->instAddr();
        ds.staticInst = microInst;
        ds.pcStateAtDecode.reset(_macroPC->clone());
        ds.instSize = static_cast<uint8_t>(_curMacro->size());
        ds.isLastInMacro = microInst->isLastMicroop();

        DPRINTF(Signal3CPUDecode,
                "macro micro-op @ pc=%#x.%u "
                "(isLast=%d)\n",
                ds.addr, (unsigned)upc, (int)ds.isLastInMacro);
        _cpu.pipeline().lineTraceEvent("D.microOp");

        // Advance _macroPC: this either bumps microPC for the next
        // micro-op, or (on isLastMicroop) advances instAddr past the
        // macro and resets microPC to 0.
        microInst->advancePC(*_macroPC);

        if (microInst->isLastMicroop()) {
            _curMacro = nullptr;
            _macroPC.reset();
        }

        // tryEarlyResolveBranch is safe to call on micro-ops; it
        // bails on non-control (the typical LDM/STM micro-op) and
        // on indirect-control (POP {pc}'s last micro-op).
        tryEarlyResolveBranch(ds);

        dSlot.in().write(std::optional<DecodedSlot>{ds});
        _decodedThisCycle = true;
        return;
    }

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
            // The word we need depends on which slot of the streaming
            // decoder we're filling:
            //   - Fresh start (no word loaded yet, or decoder has
            //     consumed everything): need the 4-byte word covering
            //     _nextInstrAddrToDeliver, i.e. addr & ~3.
            //   - Continuation of a 32-bit Thumb-2 (decoder previously
            //     fed lo half from word W, now needs hi half): the
            //     next word at W + 4.
            const Addr expectedWordAddr = _haveLoadedWord
                ? (_lastFetchPc + sizeof(uint32_t))
                : (_nextInstrAddrToDeliver & ~Addr(0x3));
            if (w.addr < expectedWordAddr) {
                // R6 wrong-path filter: word covers bytes wholly below
                // what we need — pop and look at the next word.
                DPRINTF(Signal3CPUDecode,
                        "skip stale word %#x (below expected %#x); "
                        "pop\n",
                        w.addr, expectedWordAddr);
                d_pop_request.write(true);
                return;
            }
            if (w.addr > expectedWordAddr) {
                // Out-of-order response: the word we need is still in
                // flight (or queued behind an unsent fetch).  Wait
                // WITHOUT popping — absorbStaging's ordered-insert
                // will place the missing word at the head when it
                // arrives, and the pipeline retick re-runs settle().
                DPRINTF(Signal3CPUDecode,
                        "wait: head word %#x is above expected %#x "
                        "(awaiting reorder)\n",
                        w.addr, expectedWordAddr);
                return;
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
    ds.pcStateAtDecode.reset(instrPc->clone());

    DPRINTF(Signal3CPUDecode,
            "decoded inst @ pc=%#x size=%u isControl=%d isCondCtrl=%d "
            "isIndirectCtrl=%d isMacroop=%d\n",
            ds.addr, staticInst->size(),
            (int)staticInst->isControl(),
            (int)staticInst->isCondCtrl(),
            (int)staticInst->isIndirectCtrl(),
            (int)staticInst->isMacroop());

    ds.instSize = static_cast<uint8_t>(staticInst->size());

    // Macro-op detection: if this is a macro, set up expansion state
    // and emit the FIRST micro-op (microPC=0).  Subsequent micro-ops
    // are emitted by the section 1.5 block on later cycles.  The
    // macro's own fall-through addr is set into _nextInstrAddrToDeliver
    // here (so F can keep prefetching past the macro while D streams
    // its remaining micro-ops out of the cached _curMacro).
    if (staticInst->isMacroop()) {
        _curMacro = staticInst;
        _macroPC.reset(instrPc->clone());   // microPC=0 already

        StaticInstPtr microInst = _curMacro->fetchMicroop(0);

        ds.staticInst = microInst;
        ds.isLastInMacro = microInst->isLastMicroop();
        // ds.instSize already set above to staticInst (the macro)'s size.

        DPRINTF(Signal3CPUDecode,
                "macro 1st micro-op @ pc=%#x.0 isLast=%d\n",
                ds.addr, (int)ds.isLastInMacro);
        _cpu.pipeline().lineTraceEvent("D.decodeMacro");

        // Advance _macroPC by this first micro-op.
        microInst->advancePC(*_macroPC);
        if (microInst->isLastMicroop()) {
            // Single-microop macro (rare); finalize.
            _curMacro = nullptr;
            _macroPC.reset();
        }
    } else {
        ds.staticInst = staticInst;
        ds.isLastInMacro = true;  // non-macro inst always "completes"
        _cpu.pipeline().lineTraceEvent("D.decode");
    }

    _nextInstrAddrToDeliver = ds.addr + staticInst->size();
    _decodedThisCycle = true;

    // Try to early-resolve a direct unconditional branch from
    // its immediate offset.  Updates ds.earlyResolved +
    // ds.resolvedNpc and pulses d_redirect_to_f when the branch
    // target differs from the fall-through.  Conditional and
    // indirect branches return immediately and resolve at E.
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
    // Only direct unconditional branches (B, BL) resolve at D —
    // they need only the immediate offset.  Cortex-M4 has no
    // D-side flag forwarding, so conditional branches must wait
    // for E (TRM Table 3-1; CALIBRATION_HANDOFF.md notes
    // "no D-side bypass").  Indirect branches need register reads
    // and also resolve at E.
    if (ds.staticInst->isCondCtrl())
        return false;
    if (ds.staticInst->isIndirectCtrl())
        return false;

    // Speculatively run the unconditional branch's execute()
    // against the current thread context.  Save/restore
    // tc.pcState() around the call.  Unconditional B/BL ends any
    // IT block per ARMv7-M ARM A7.7.5, so ITSTATE side-effects
    // are bounded; the savedPc restore preserves the pre-execute
    // architectural state until E commits via the earlyResolved
    // fast path.
    SimpleThread &thread = *_cpu.thread(0);
    std::unique_ptr<PCStateBase> savedPc(thread.pcState().clone());
    thread.pcState(*ds.pcStateAtDecode);
    ExecContext ctx(_cpu, thread);
    Fault fault = ds.staticInst->execute(&ctx, /*traceData=*/nullptr);
    panic_if(fault != NoFault,
             "Decode::tryEarlyResolveBranch: fault from execute()");
    ds.staticInst->advancePC(thread.getTC());
    const Addr resolvedNpc = thread.pcState().instAddr();
    thread.pcState(*savedPc);

    ds.earlyResolved = true;
    ds.resolvedNpc = resolvedNpc;

    const Addr fallThrough = ds.addr + ds.instSize;
    if (resolvedNpc != fallThrough) {
        DPRINTF(Signal3CPUDecode,
                "early-resolve TAKEN pc=%#x -> %#x\n",
                ds.addr, resolvedNpc);
        _cpu.pipeline().lineTraceEvent("D.earlyRes-T");
        d_redirect_to_f.write(std::optional<Addr>{resolvedNpc});
        // D's own internal stream redirects: future decodes start
        // at the new target.  The decoder cache must be invalidated
        // since the target word's bytes are different.
        _nextInstrAddrToDeliver = resolvedNpc;
        _haveLoadedWord = false;
        _lastFetchPc = 0;
    } else {
        // Edge case: `b .` (infinite loop where target == fallThrough).
        DPRINTF(Signal3CPUDecode,
                "early-resolve NOT-TAKEN pc=%#x (target=fall-through)\n",
                ds.addr);
        _cpu.pipeline().lineTraceEvent("D.earlyRes-NT");
    }
    return true;
}

std::string
Decode::snapshotString() const
{
    std::ostringstream os;
    os << "next=0x" << std::hex << _nextInstrAddrToDeliver
       << " slot=";
    if (dSlot.out().valid() && dSlot.out().read().has_value()) {
        const auto &s = *dSlot.out().read();
        os << "v(0x" << std::hex << s.addr
           << " sz=" << std::dec << (unsigned)s.instSize
           << " last=" << (int)s.isLastInMacro;
        if (s.earlyResolved) {
            os << " eR->0x" << std::hex << s.resolvedNpc;
        }
        os << ")";
    } else {
        os << "empty";
    }
    if (_curMacro) {
        os << " macro@uop" << std::dec << (unsigned)currentMicroPC();
    }
    return os.str();
}

} // namespace signal3
} // namespace gem5
