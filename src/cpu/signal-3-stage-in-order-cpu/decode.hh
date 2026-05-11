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

#ifndef __CPU_SIGNAL_3_STAGE_IN_ORDER_CPU_DECODE_HH__
#define __CPU_SIGNAL_3_STAGE_IN_ORDER_CPU_DECODE_HH__

#include <optional>

#include "base/types.hh"
#include "cpu/signal-3-stage-in-order-cpu/signal.hh"
#include "cpu/signal-3-stage-in-order-cpu/types.hh"

namespace gem5
{
class ThreadContext;

namespace signal3
{

class SignalCPU;
class Fetch;

/**
 * Decode (D) stage.
 *
 * Subscribes to F's f_word_out (so re-fires when a new word lands)
 * and to e_accept_d (so re-fires when E goes back from blocked to
 * accepting).  Drives the gem5 generic InstDecoder, which is Thumb-
 * aware: a single 32-bit fetch word may produce two 16-bit Thumb
 * instructions (e.g. two NOPs encoded as 0xBF00BF00), in which case
 * D decodes twice without popping the FIFO between them.  When the
 * decoder asks for more bytes, D pulses d_pop_request so F advances
 * its FIFO head.
 *
 * S2 scope: decoder driving + dSlot Latch + d_pop_request.  No E->D
 * flag forwarding (S5), no early branch resolution (S5), no E (yet).
 * For S2 the Pipeline drives e_accept_d=true unconditionally so D
 * never stalls.
 */
class Decode : public Stage
{
  public:
    Decode(SignalGraph &g, SignalCPU &cpu, Fetch &fetch);

    void settle() override;

    /* ---- Reset on entry-point change (called from SignalCPU) ---- */
    void resetTo(Addr entryPc);

    /* ---- Per-cycle re-arm hook (called by Pipeline::evaluate) ---- */
    void beginCycle();

    /* ---- Outputs ---- */
    Latch<std::optional<DecodedSlot>> dSlot;
    Signal<bool>                      d_pop_request;
    Signal<std::optional<Addr>>       d_redirect_to_f;  // S5

    /* ---- Input wires (bound by Pipeline ctor) ---- */
    void setEAcceptInput(Signal<bool> *s) { _eAcceptInput = s; }
    void setERedirectInput(Signal<std::optional<Addr>> *s)
    { _eRedirectInput = s; }
    void setEFlagsInput(Signal<ForwardedFlags> *s)
    { _eFlagsInput = s; }

    /* ---- Apply redirect from E (resets decoder & dSlot) ---- */
    void applyRedirect(Addr target);

    /* ---- Read-only query helpers ---- */
    Addr nextInstrAddrToDeliver() const { return _nextInstrAddrToDeliver; }
    bool haveLoadedWord() const { return _haveLoadedWord; }
    Addr lastFetchPc() const { return _lastFetchPc; }
    bool inMacroExpansion() const { return (bool)_curMacro; }
    MicroPC currentMicroPC() const {
        return _macroPC ? _macroPC->microPC() : 0;
    }

    /** True if D's latched output (dSlot.out()) currently holds a
     *  decoded slot that E hasn't accepted yet — meaning D->E has
     *  pending work to flow next cycle. */
    bool hasLatchedOutput() const
    {
        return dSlot.out().valid() && dSlot.out().read().has_value();
    }

    /** True if D has just written a slot to dSlot.in() that will
     *  become visible to E at the NEXT clock edge.  Used by the
     *  clock-gating heuristic so the pipeline doesn't idle between
     *  the cycle D produces a slot and the cycle E commits it.
     *  Without this gate, when F finishes a stop-at-branch fetch and
     *  D decodes the second 16-bit inst of a same-word pair, the
     *  pipeline would stop because dSlot.out() is still empty (the
     *  new slot is only in dSlot.in() until the next edge).
     *
     *  Non-const because Latch::in() returns a non-const reference;
     *  this is a read-only check on the input signal's value. */
    bool hasPendingInputSlot()
    {
        return dSlot.in().valid() && dSlot.in().read().has_value();
    }

    /** One-line printable state for the line-trace tables. */
    std::string snapshotString() const;

  private:
    SignalCPU &_cpu;
    Fetch &_fetch;

    /* Decoder state across cycles.  These mirror the predecessor's
     * Decode::_haveLoadedWord/_lastFetchPc/_nextInstrAddrToDeliver
     * fields.  They're plain members rather than Latches because
     * they update at most once per cycle (in settle()) and don't
     * need a separate sample/commit step. */
    Addr _nextInstrAddrToDeliver = 0;
    bool _haveLoadedWord = false;
    Addr _lastFetchPc = 0;

    /* Single-issue throttle: at most one instruction decoded per
     * clock cycle.  Reset by beginCycle() at the top of evaluate(). */
    bool _decodedThisCycle = false;

    /* The consume-vs-hold decision for the current dSlot.out() is
     * made on the first settle() fire of the cycle and locked in.
     * Subsequent re-fires (e.g. e_accept_d toggling because E went
     * Pending mid-cycle) must NOT flip the decision — that would
     * overwrite a freshly-decoded successor slot.  Reset in
     * beginCycle(). */
    bool _slotDecisionMadeThisCycle = false;
    bool _slotDecidedConsumed = false;

    /* Bound by Pipeline ctor to Execute's e_accept_d output. */
    Signal<bool> *_eAcceptInput = nullptr;

    /* Bound by Pipeline ctor to Execute's e_redirect_to_f output. */
    Signal<std::optional<Addr>> *_eRedirectInput = nullptr;

    /* Bound by Pipeline ctor to Execute's e_flags_to_d output.
     * Pulse signal carrying post-commit NZCV; D reads it on
     * settle re-fires triggered by E's commit so a same-cycle
     * 16-bit T1 Bcond can resolve at D against just-committed
     * flags instead of redirecting at E. */
    Signal<ForwardedFlags> *_eFlagsInput = nullptr;

    /* Address of an unresolved 16-bit T1 Bcond that D decoded
     * this cycle but couldn't resolve because forwarded flags
     * weren't valid yet (D fires at stageId=1 before E at
     * stageId=2).  Set by tryDecodeOnlyResolveCondBranch when it
     * defers; cleared on successful re-resolution, on
     * applyRedirect, and at beginCycle.  When E pulses
     * e_flags_to_d it re-queues D; D's settle then retries the
     * resolve via retryPendingCondBranchResolve(). */
    std::optional<Addr> _pendingCondBranchAddr;

    /* Cached fall-through address for the pending cond branch.
     * Used by retryPendingCondBranchResolve() to recover the
     * full slot from dSlot.in() and re-apply resolution. */
    Addr _pendingCondBranchFallThrough = 0;

    /* Re-attempt resolution of a cond branch that was decoded
     * earlier in this same cycle but couldn't resolve because
     * forwarded flags hadn't arrived yet.  Called at the top of
     * settle() when _pendingCondBranchAddr is set and the
     * forwarded-flags signal has since pulsed. */
    void retryPendingCondBranchResolve();

    /* True iff dSlot.out() holds an instruction that writes
     * MISCREG_CPSR — i.e. a flag-setter that E may commit this
     * cycle.  Used to gate the CPSR misc-reg fallback in
     * tryDecodeOnlyResolveCondBranch: when this returns true and
     * forwarded flags aren't yet valid, the misc-reg holds
     * pre-commit (stale) flags so we must defer resolution. */
    bool dSlotOutHasPendingFlagSetter() const;

    /* Try to early-resolve a just-decoded direct unconditional
     * branch from its immediate offset.  Returns true if
     * resolution happened (slot marked earlyResolved, possibly
     * d_redirect_to_f written).  Indirect branches go to E
     * (need register reads).  Conditional branches resolve via
     * tryDecodeOnlyResolveCondBranch (16-bit T1 Bcond) or fall
     * through to E (32-bit T3 B.W cond). */
    bool tryEarlyResolveBranch(DecodedSlot &ds);

    /** Decode-only resolution for 16-bit T1 Bcond.  Reads cond
     *  field + imm8 from the inst encoding, evaluates against
     *  current NZCV from CPSR, computes target — no execute()
     *  call, no thread-state mutation.  Returns true if the
     *  branch was resolved (ds.earlyResolved set, possibly
     *  d_redirect_to_f pulsed); false if the inst isn't a
     *  16-bit T1 Bcond and should be resolved at E. */
    bool tryDecodeOnlyResolveCondBranch(DecodedSlot &ds);

    /** Decode-only resolution for `bx lr` (M-profile BxMProfile).
     *  Reads LR via `tc->getReg(srcRegIdx(0))`, checks the EXC_RETURN
     *  magic-value gate and the Thumb-bit gate, then on success
     *  marks the slot earlyResolved and pulses `d_redirect_to_f`
     *  with `target = lr & ~1`.  No execute() call, no
     *  thread-state mutation.  Falls through to E (return false)
     *  on:
     *    - non-bx_lr indirect branches (not one int-reg source),
     *    - EXC_RETURN magic LR value (`(lr & 0xFFFFFFF0) == 0xFFFFFFF0`),
     *    - Thumb-bit fault case (`(lr & 1) == 0`),
     *    - in-flight writer of LR at D or E this cycle.
     *  Skips `isCall()` indirect branches (BLX Rm) — those write LR
     *  as a side-effect, which the earlyResolved fast-path skips. */
    bool tryDecodeOnlyResolveBxLr(DecodedSlot &ds);

    /** True iff `dSlot.out()` holds an inst that writes the given
     *  integer register (by `intRegClass` index).  Used by the
     *  `bx lr` decode-time resolve to defer when a same-cycle
     *  D->E inst will write LR before bx_lr commits.  Mirrors
     *  `dSlotOutHasPendingFlagSetter` but scoped to int regs. */
    bool dSlotOutWritesIntReg(RegIndex idx) const;

    /** True iff `Execute::_eSlot` holds an inst that writes the
     *  given integer register.  Used by the `bx lr` decode-time
     *  resolve to defer when E is about to commit an LR-writer
     *  this cycle (D's `tc->getReg` would see pre-commit value). */
    bool eSlotInstWritesIntReg(RegIndex idx) const;

    /** Pending-resolution state for a deferred `bx lr`.  Set when
     *  `tryDecodeOnlyResolveBxLr` defers because of an in-flight
     *  LR writer; cleared on successful resolution / applyRedirect /
     *  beginCycle.  Today only used for tracing; D's slot stays in
     *  `dSlot.in()` and naturally re-resolves on the next cycle. */
    std::optional<Addr> _pendingBxLrAddr;

    /* ---- Macro-op micro-op expansion (Minor-style) -----------------
     *
     * ARM Thumb-2 LDM/STM/PUSH/POP and predicated multi-reg ops are
     * encoded as a single PredMacroOp StaticInst whose execute()
     * panics — they MUST be expanded into micro-ops via fetchMicroop().
     *
     * D iterates the macro-op's micro-ops at exactly one per cycle
     * (single-issue), emitting each as a fresh DecodedSlot with the
     * micro-op as `staticInst`.  This mirrors MinorCPU's Decode stage
     * (cpu/minor/decode.cc) and naturally produces Cortex-M4 TRM
     * Table 3-1's "1+N" cycle count for LDM/STM/PUSH/POP via the F→D→E
     * pipeline overlap (see Pipeline path doc in DESIGN.md).
     *
     * `_curMacro` holds the macro-op while expansion is in progress.
     * `_microPC` tracks the next micro-op index to emit.
     * `_macroPC` holds a clone of the decode-time pcState used to
     * tag each emitted micro-op slot (with microPC stamped from
     * `_microPC` so E's pcStateAtDecode is correct on each cycle).
     */
    StaticInstPtr _curMacro = nullptr;
    MicroPC _microPC = 0;
    std::shared_ptr<PCStateBase> _macroPC;
};

} // namespace signal3
} // namespace gem5

#endif // __CPU_SIGNAL_3_STAGE_IN_ORDER_CPU_DECODE_HH__
