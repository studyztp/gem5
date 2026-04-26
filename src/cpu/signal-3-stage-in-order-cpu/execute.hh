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

    /* S5 forward: when E commits a flag-setting non-branch inst,
     * publish (PC, PC+size) so D can detect that the inst it just
     * decoded (a conditional branch) has its flag-producer freshly
     * committed and may early-resolve using the updated thread
     * context flags.  Pulse signal — reset each cycle. */
    Signal<std::optional<Addr>>   e_flagSetterNextPc;

    /* ---- Read-only query helpers ---- */
    bool busy() const { return _eSlot.has_value(); }
    Addr archPc() const;

  private:
    SignalCPU &_cpu;
    Decode &_decode;
    LSQ &_lsq;

    /* eSlot is plain state (not a Latch).  E mutates it in settle()
     * and the value is observed only by E itself across cycles, so a
     * Latch would be redundant. */
    std::optional<DecodedSlot> _eSlot;

    /* Single-issue throttle: at most one commit per cycle. */
    bool _committedThisCycle = false;
    void beginCycleHook() { _committedThisCycle = false; }

    friend class Pipeline;   // for beginCycleHook()

    void commitOne();
};

} // namespace signal3
} // namespace gem5

#endif // __CPU_SIGNAL_3_STAGE_IN_ORDER_CPU_EXECUTE_HH__
