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

#ifndef __CPU_SIGNAL_3_STAGE_IN_ORDER_CPU_PIPELINE_HH__
#define __CPU_SIGNAL_3_STAGE_IN_ORDER_CPU_PIPELINE_HH__

#include <memory>

#include "cpu/signal-3-stage-in-order-cpu/decode.hh"
#include "cpu/signal-3-stage-in-order-cpu/execute.hh"
#include "cpu/signal-3-stage-in-order-cpu/fetch.hh"
#include "cpu/signal-3-stage-in-order-cpu/lsq.hh"
#include "cpu/signal-3-stage-in-order-cpu/signal.hh"
#include "cpu/signal-3-stage-in-order-cpu/types.hh"
#include "sim/ticked_object.hh"

namespace gem5
{
namespace signal3
{

class SignalCPU;

/**
 * Driver for the signal-driven CPU's clock-edge work.  Owns the
 * SignalGraph and all pipeline stages; on every clock edge runs:
 *
 *   beginCycle  ->  edge (commit latches)  ->  settle (combinational)
 *               ->  schedule (sendTimingReq queued packets)
 *
 * Mid-cycle async responses (from Icache/Dcache) call
 * onAsyncResponse(), which classifies the response as in-window or
 * not, drains it into the appropriate stage's staging buffer, and
 * re-runs Settle if in-window so the cascade propagates this same
 * cycle.
 */
class Pipeline : public Ticked
{
  public:
    Pipeline(SignalCPU &cpu, unsigned fifoCapacity);

    void evaluate() override;

    /* ---- Drives F's reset (called from SignalCPU::startup) ---- */
    void resetTo(Addr entryPc);

    /* ---- Async response routing ---- */
    void onAsyncResponse(const FetchWord &w, PortSide side);

    /* ---- Dcache-side async response (S3+) ---- */
    void onDcacheResponse(PacketPtr pkt);

    /* ---- Retry hand-off from IcachePort ---- */
    void retrySendPendingFetch();

    /* ---- Retry hand-off from DcachePort ---- */
    void retrySendPendingDcache();

    /* ---- Read-only accessors for ports / debug ---- */
    Fetch &getFetch() { return *_fetch; }
    Decode &getDecode() { return *_decode; }
    Execute &getExecute() { return *_execute; }
    LSQ &getLsq() { return *_lsq; }
    SignalGraph &getGraph() { return _graph; }

  private:
    SignalCPU &_cpu;
    SignalGraph _graph;
    std::unique_ptr<LSQ> _lsq;
    std::unique_ptr<Fetch> _fetch;
    std::unique_ptr<Decode> _decode;
    std::unique_ptr<Execute> _execute;

    bool _sawIcacheRetry = false;

    void scheduleOutgoing();
    bool haltConditionMet() const;
    void resetSettleGuards();

    /** Cortex-M interrupt entry.  At a safe pipeline point (no LSQ
     *  Pending, no in-flight wrong-path that would corrupt arch
     *  state), invoke any pending interrupt fault.  This pushes the
     *  exception stack frame via the MMU's stacking-barrier and
     *  redirects PC to the vector handler.  We then flush F/D so
     *  the post-vector instruction stream replaces the speculatively
     *  fetched/decoded one. */
    void checkAndTakeInterrupts();

    /** Cortex-M EXC_RETURN handling.  When `bx lr` (or any load-to-PC)
     *  resolves to a value in the 0xFFFFFFF0..0xFFFFFFFF range, the
     *  Cortex-M architecture interprets it as an exception-return
     *  request: pop the exception frame, restore the saved PC, and
     *  deactivate the IRQ in the SCS.  In gem5's standard CPUs, this
     *  is intercepted by the MMU translate path before the fetch
     *  reaches memory; SignalCPU bypasses the MMU for fetches, so we
     *  detect it here and dispatch directly to MProfileInterrupts. */
    void checkExcReturn();
};

} // namespace signal3
} // namespace gem5

#endif // __CPU_SIGNAL_3_STAGE_IN_ORDER_CPU_PIPELINE_HH__
