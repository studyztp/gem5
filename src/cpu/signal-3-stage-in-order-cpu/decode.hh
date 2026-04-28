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

    /* Try to early-resolve a just-decoded direct unconditional
     * branch from its immediate offset.  Returns true if
     * resolution happened (slot marked earlyResolved, possibly
     * d_redirect_to_f written).  Cortex-M4 has no D-side flag
     * forwarding, so conditional and indirect branches go to E. */
    bool tryEarlyResolveBranch(DecodedSlot &ds);

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
