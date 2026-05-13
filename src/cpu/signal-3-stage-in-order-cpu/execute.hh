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

#ifndef __CPU_SIGNAL_3_STAGE_IN_ORDER_CPU_EXECUTE_HH__
#define __CPU_SIGNAL_3_STAGE_IN_ORDER_CPU_EXECUTE_HH__

#include <optional>

#include "base/types.hh"
#include "cpu/signal-3-stage-in-order-cpu/alu_fu.hh"
#include "cpu/signal-3-stage-in-order-cpu/decode.hh"
#include "cpu/signal-3-stage-in-order-cpu/lsq.hh"
#include "cpu/signal-3-stage-in-order-cpu/signal.hh"
#include "cpu/signal-3-stage-in-order-cpu/types.hh"

namespace gem5
{
namespace signal3
{

class SignalCPU;

/**
 * Execute (E) stage.
 *
 * Subscribes to:
 *   - D's dSlot.out() so re-fires when a new instruction is latched
 *   - LSQ's completionSignal so re-fires when a memory response lands
 *
 * Outputs:
 *   - e_accept_d: combinational back-pressure into D.  True iff E
 *     can accept a new slot this cycle (i.e. LSQ is Idle and the
 *     current eSlot is empty or has been retired this cycle).
 *   - e_redirect_to_f: placeholder pulse Signal for indirect/computed
 *     branches that must redirect F.  Empty in S3 — wired up in S4.
 *   - e_flags_out: placeholder for E->D flag forwarding.  Inert in
 *     S3/S4 — populated in S5.
 *
 * Latched state:
 *   - eSlot: the in-flight instruction.  Held across cycles when the
 *     LSQ is Pending.
 *
 * S3 scope: 4-path dispatch (LSQ stall / LSQ complete / memref-issue /
 * non-mem) ported from the predecessor's Execute::runOneCycle.  No
 * branch redirects yet (S4) and no flag forwarding (S5).  The
 * earlyResolved fast path is defined here but is dead code in S3
 * (Decode never sets earlyResolved=true until S5).
 */
class Execute : public Stage
{
  public:
    Execute(SignalGraph &g, SignalCPU &cpu, Decode &decode, LSQ &lsq);

    void settle() override;

    void resetTo(Addr entryPc);

    /* ---- Outputs ---- */
    Signal<bool>                  e_accept_d;
    Signal<std::optional<Addr>>   e_redirect_to_f;  // S4+

    /* Pulsed once per commit with the post-commit NZCV.  D
     * subscribes so it can re-resolve a same-cycle dependent
     * 16-bit T1 Bcond using just-committed flags instead of
     * waiting for E to commit the branch itself. */
    Signal<ForwardedFlags>        e_flags_to_d;

    /* ---- Read-only query helpers ---- */
    bool busy() const { return _eSlot.has_value(); }
    Addr archPc() const;
    bool slotValid() const { return _eSlot.has_value(); }
    Addr slotPc() const { return _eSlot.has_value() ? _eSlot->addr : 0; }

    /** Read-only accessor for the StaticInst currently in `_eSlot`,
     *  or nullptr if E is empty.  Used by D's
     *  `eSlotInstWritesIntReg()` to detect an in-flight writer of LR
     *  before speculatively reading LR for `bx lr` decode-time
     *  resolution.  See decode.cc:tryDecodeOnlyResolveBxLr. */
    StaticInstPtr eSlotInst() const
    {
        return _eSlot.has_value() ? _eSlot->staticInst : nullptr;
    }

    /** True if E most recently committed a non-last micro-op of a
     *  macro and has not yet committed the macro's last uop.  The
     *  macro is "half-committed": SP / regs may have been advanced
     *  by the already-committed uops, but the last uop (which
     *  typically does the indirect branch / SP writeback) hasn't
     *  fired yet.  Taking an IRQ here would drop the in-flight last
     *  uop via applyRedirect; on EXC_RETURN the macro re-runs from
     *  uop 0 with the partial state already applied, reading from
     *  wrong stack addresses (see issues/2026-05-10-pushpop_v2-irq-race/).
     *  Real Cortex-M4 silicon defers IRQ entry on `pop {…, pc}` for
     *  the same reason — the architecture cannot encode mid-pop SP
     *  state into EPSR.ICI.  Used by the Pipeline IRQ gate.  Cleared
     *  on resetTo()/applyRedirect for safety. */
    bool inMidMacro() const { return _midMacro; }

    /** One-line printable state for the line-trace tables.  When
     *  `detailed=true` (Signal3CPULineTraceDetail), appends the
     *  full `staticInst->disassemble(pc)` text after the commit /
     *  held inst's mnemonic. */
    std::string snapshotString(bool detailed = false) const;

  private:
    SignalCPU &_cpu;
    Decode &_decode;
    LSQ &_lsq;

    /* eSlot is plain state (not a Latch).  E mutates it in settle()
     * and the value is observed only by E itself across cycles, so a
     * Latch would be redundant. */
    std::optional<DecodedSlot> _eSlot;

    /* Set true on commit of a uop with isLastInMacro=false; cleared
     * on commit of a uop with isLastInMacro=true (or on
     * resetTo()/applyRedirect).  Read by Pipeline::checkAndTakeInterrupts
     * to defer IRQs while a macro is half-committed.  See
     * inMidMacro() comment above. */
    bool _midMacro = false;

    /* Integer-ALU function unit.  1-cy ops (nop/mov/add/sub/cmp/mul)
     * take the same-cycle pass-through path; SDIV/UDIV get a dynamic
     * 2-12 cy latency via a scheduled completion event (see alu_fu.cc
     * latencyFor()).  Floating-point ops fall through to the legacy
     * direct-execute path in commitOne(). */
    AluFunctionUnit _alu;

    /* Single-issue throttle: at most one commit per cycle. */
    bool _committedThisCycle = false;
    /* When `_committedThisCycle` is set, these hold the address +
     * size of the instruction that committed this cycle. Used by
     * `snapshotString()` so the Signal3CPULineTrace summary table's
     * execute column shows `slot=v(0x<pc> sz=<N>)` on commit cycles
     * (eSlot itself is reset on commit and would otherwise read
     * `slot=empty` for the end-of-cycle snapshot). */
    Addr _committedAddrThisCycle = 0;
    uint8_t _committedSizeThisCycle = 0;
    /* StaticInst of the committed inst (for snapshotString mnemonic).
     * Cleared each beginCycleHook; set in commitOne(). */
    StaticInstPtr _committedInstThisCycle;

    /* Redirect attribution for the line trace.  Set in commitOne()
     * at the same site as `e_redirect_to_f.write()`; lets the
     * trace show BOTH the branch PC that caused the redirect AND
     * the target.  Cleared each beginCycleHook. */
    struct RedirectInfo
    {
        Addr branchPc;
        Addr target;
    };
    std::optional<RedirectInfo> _redirectThisCycle;

    void beginCycleHook()
    {
        _committedThisCycle = false;
        _committedAddrThisCycle = 0;
        _committedSizeThisCycle = 0;
        _committedInstThisCycle = nullptr;
        _redirectThisCycle.reset();
        // ALU FU is event-driven: a multi-cy op's completion fires
        // before this cycle's settle (event priority < CPU_Tick_Pri),
        // so the FU state is current without a per-cycle tick.
    }

    friend class Pipeline;   // for beginCycleHook()

    void commitOne();
};

} // namespace signal3
} // namespace gem5

#endif // __CPU_SIGNAL_3_STAGE_IN_ORDER_CPU_EXECUTE_HH__
