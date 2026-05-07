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

#include "arch/arm/insts/static_inst.hh"
#include "arch/arm/regs/cc.hh"
#include "arch/arm/utility.hh"
#include "arch/generic/decoder.hh"
#include "arch/generic/pcstate.hh"
#include "cpu/signal-3-stage-in-order-cpu/exec_context.hh"
#include "cpu/signal-3-stage-in-order-cpu/fetch.hh"
#include "cpu/signal-3-stage-in-order-cpu/pipeline.hh"
#include "cpu/signal-3-stage-in-order-cpu/signal_cpu.hh"
#include "cpu/static_inst.hh"
#include "cpu/thread_context.hh"
#include "debug/Signal3CPUDecode.hh"
#include "debug/Signal3CPUFlagsFwd.hh"
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
    _pendingCondBranchAddr.reset();
    _pendingCondBranchFallThrough = 0;
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
    // Any unresolved cond branch we were waiting to re-resolve is
    // wrong-path now — drop the pending state.
    _pendingCondBranchAddr.reset();
    _pendingCondBranchFallThrough = 0;
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

    // 0a. If we deferred a cond-branch resolution earlier this cycle
    //     because forwarded flags weren't yet available, retry now
    //     that E may have pulsed.  This re-fire is the SignalGraph's
    //     way of giving us E-after-D ordering for the in-cycle
    //     flag-setter -> dependent-bcc case.
    retryPendingCondBranchResolve();

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

    // Conditional branches: 16-bit T1 Bcond resolves at D using
    // flags forwarded from E's same-cycle commit (see
    // tryDecodeOnlyResolveCondBranch).  32-bit T3 B.W cond and
    // any condition we can't decode-only resolve fall through to
    // E.  Note tryDecodeOnlyResolveCondBranch may set
    // _pendingCondBranchAddr and return false if forwarded
    // flags haven't arrived yet — D's settle re-fire on the
    // e_flags_to_d pulse will retry.
    if (ds.staticInst->isCondCtrl())
        return tryDecodeOnlyResolveCondBranch(ds);

    // Indirect branches need register reads via the
    // operand-forwarding path which is E-side.
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

bool
Decode::tryDecodeOnlyResolveCondBranch(DecodedSlot &ds)
{
    // 16-bit T1 Bcond decode-only resolution using flags
    // FORWARDED from E's same-cycle commit.
    //
    // Encoding (ARMv7-M ARM A7.7.12, T1):
    //     1101 cccc imm8     where cond cccc != 0xE/0xF
    // PC-relative target = (ds.addr + 4) + sign_extend(imm8 << 1).
    //
    // Per ARM ARM, T1 Bcond cannot appear inside an IT block, so
    // the encoding's cond field is the actual condition (no
    // ITSTATE override) — we can resolve without touching any
    // thread state.
    //
    // Crucially, we read NZCV from the e_flags_to_d FORWARDING
    // signal, NOT from MISCREG_CPSR.  The SignalGraph dispatches
    // stages in stageId order (D=1 fires before E=2), so on D's
    // first fire of a cycle the CPSR misc-reg holds pre-commit
    // flags — wrong if a flag-setter is being committed at E
    // this cycle.  The earlier decode-only-with-misc-reg attempt
    // failed exactly this way (CopyDataInit loop ran
    // indefinitely with stale flags).  When forwarded flags
    // aren't yet available, we DEFER resolution: store the slot's
    // address in _pendingCondBranchAddr and return false.  E's
    // commitOne() then pulses e_flags_to_d, which re-queues D;
    // D's next settle pass calls retryPendingCondBranchResolve()
    // and finishes the resolution with valid forwarded flags.
    if (ds.instSize != 2)
        return false;  // 32-bit B.W cond falls back to E-side

    auto *armInst = dynamic_cast<gem5::ArmISA::ArmStaticInst *>(
        ds.staticInst.get());
    if (!armInst)
        return false;

    const uint16_t enc = (uint16_t) armInst->encoding();
    if ((enc & 0xF000) != 0xD000)
        return false;
    const unsigned cond = (enc >> 8) & 0xF;
    if (cond == 0xE || cond == 0xF)
        return false;  // AL or invalid — not a real Bcond

    const Addr fallThrough = ds.addr + ds.instSize;

    // Pick the flag source.  Three cases:
    //
    //   (1) Forwarded flags valid → E has already committed this
    //       cycle (typical on D's settle re-fire after E's
    //       e_flags_to_d pulse).  Always preferred.
    //
    //   (2) Forwarded flags invalid AND dSlot.out() does NOT hold
    //       a pending flag-setter → misc-reg is up-to-date from a
    //       prior cycle's commit.  Safe.  Covers the common
    //       cross-cycle case (subs in cycle T-1, bne decoded in
    //       cycle T; in cycle T's settle E commits something else
    //       or nothing, so no in-cycle flag-setter pulse arrives).
    //
    //   (3) Forwarded flags invalid AND dSlot.out() holds a
    //       pending flag-setter that E will commit this cycle but
    //       hasn't yet (D fires at stageId=1 before E at
    //       stageId=2) → misc-reg is stale.  Defer; the
    //       e_flags_to_d pulse from E's commit will re-queue D
    //       and retryPendingCondBranchResolve() finishes us.
    ForwardedFlags ff;
    if (_eFlagsInput && _eFlagsInput->valid()) {
        ff = _eFlagsInput->read();
    } else if (!dSlotOutHasPendingFlagSetter()) {
        // ARM stores NZCV in dedicated CC regs (cc_reg::Nz / C / V),
        // not MISCREG_CPSR.  Flag-setting insts write these regs
        // when they execute, so reading them here gets the
        // architectural NZCV state from the most recent commit.
        ThreadContext *tc = _cpu.thread(0)->getTC();
        ff.nz = (uint8_t) tc->getReg(gem5::ArmISA::cc_reg::Nz);
        ff.c  = (bool)    tc->getReg(gem5::ArmISA::cc_reg::C);
        ff.v  = (bool)    tc->getReg(gem5::ArmISA::cc_reg::V);
    } else {
        _pendingCondBranchAddr = ds.addr;
        _pendingCondBranchFallThrough = fallThrough;
        DPRINTF(Signal3CPUDecode,
                "defer cond-branch resolve pc=%#x (in-cycle "
                "flag-setter pending at E)\n", ds.addr);
        return false;
    }

    const bool taken = gem5::ArmISA::testPredicate(
        ff.nz, ff.c, ff.v,
        (gem5::ArmISA::ConditionCode) cond);
    DPRINTF(Signal3CPUFlagsFwd,
            "resolve check pc=%#x cond=%u nz=%u c=%u v=%u taken=%d\n",
            ds.addr, cond, (unsigned)ff.nz,
            (unsigned)ff.c, (unsigned)ff.v, (int)taken);

    // Branch target: PC-relative from PC+4.
    const int8_t imm8 = (int8_t) (enc & 0xFF);
    const int32_t offset = static_cast<int32_t>(imm8) << 1;
    const Addr target = taken ? (ds.addr + 4 + offset) : fallThrough;

    ds.earlyResolved = true;
    ds.resolvedNpc = target;
    _pendingCondBranchAddr.reset();
    _pendingCondBranchFallThrough = 0;

    const char *src =
        (_eFlagsInput && _eFlagsInput->valid()) ? "fwd" : "miscreg";
    if (taken) {
        DPRINTF(Signal3CPUDecode,
                "decode-only resolve TAKEN pc=%#x cond=%u -> %#x "
                "(%s)\n", ds.addr, cond, target, src);
        _cpu.pipeline().lineTraceEvent("D.earlyRes-T");
        d_redirect_to_f.write(std::optional<Addr>{target});
        // D's own internal stream redirects: future decodes start
        // at the new target.  The decoder cache must be invalidated.
        _nextInstrAddrToDeliver = target;
        _haveLoadedWord = false;
        _lastFetchPc = 0;
    } else {
        DPRINTF(Signal3CPUDecode,
                "decode-only resolve NOT-TAKEN pc=%#x cond=%u "
                "(%s)\n", ds.addr, cond, src);
        _cpu.pipeline().lineTraceEvent("D.earlyRes-NT");
        // No redirect — D continues sequentially.
    }
    return true;
}

bool
Decode::dSlotOutHasPendingFlagSetter() const
{
    if (!dSlot.out().valid() || !dSlot.out().read().has_value())
        return false;
    const StaticInstPtr &inst = dSlot.out().read()->staticInst;
    if (!inst)
        return false;
    // ARM flag-setting insts (subs, cmps, adds, ...) write the
    // dedicated CC regs (cc_reg::Nz / C / V), not MISCREG_CPSR.
    // Some insts only write a subset (e.g. shifts that only
    // update C), so check for ANY of Nz/C/V as a dest.
    const uint8_t n = inst->numDestRegs();
    for (uint8_t i = 0; i < n; ++i) {
        const RegId &dest = inst->destRegIdx(i);
        if (!dest.is(CCRegClass))
            continue;
        const RegIndex idx = dest.index();
        if (idx == gem5::ArmISA::cc_reg::_NzIdx
                || idx == gem5::ArmISA::cc_reg::_CIdx
                || idx == gem5::ArmISA::cc_reg::_VIdx) {
            return true;
        }
    }
    return false;
}

void
Decode::retryPendingCondBranchResolve()
{
    if (!_pendingCondBranchAddr.has_value())
        return;
    if (!_eFlagsInput || !_eFlagsInput->valid())
        return;  // E hasn't pulsed yet this cycle — wait for re-fire

    // The slot we deferred on should still be in dSlot.in() — D
    // is single-issue per cycle so nothing has overwritten it.
    if (!dSlot.in().valid() || !dSlot.in().read().has_value()) {
        // Lost — should not happen given single-issue, but bail
        // safely rather than panic.  The cond branch falls through
        // to E-side resolution.
        _pendingCondBranchAddr.reset();
        _pendingCondBranchFallThrough = 0;
        return;
    }

    DecodedSlot ds = *dSlot.in().read();
    if (ds.addr != *_pendingCondBranchAddr || ds.earlyResolved) {
        // Slot was overwritten or already resolved — clean up
        // and skip.
        _pendingCondBranchAddr.reset();
        _pendingCondBranchFallThrough = 0;
        return;
    }

    // Re-run the resolution.  tryDecodeOnlyResolveCondBranch will
    // see _eFlagsInput->valid() this time and finalize ds.
    if (tryDecodeOnlyResolveCondBranch(ds)) {
        // Rewrite dSlot.in() with the now-resolved slot so E's
        // earlyResolved fast path commits it without redirecting.
        // Signal value-equality detects the change (earlyResolved
        // / resolvedNpc differ) and cascades to E.
        dSlot.in().write(std::optional<DecodedSlot>{ds});
        DPRINTF(Signal3CPUDecode,
                "retry resolved pc=%#x earlyResolved=1 npc=%#x\n",
                ds.addr, ds.resolvedNpc);
    }
    // _pendingCondBranchAddr was cleared by the successful path
    // inside tryDecodeOnlyResolveCondBranch; if it failed (eg the
    // signal cleared between fires — unlikely), we'll retry on the
    // next fire or fall through to E.
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
