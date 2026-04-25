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

#ifndef __CPU_SIMPLE_3_CYCLE_IN_ORDER_CPU_PIPELINE_HH__
#define __CPU_SIMPLE_3_CYCLE_IN_ORDER_CPU_PIPELINE_HH__

#include <memory>

#include "base/types.hh"
#include "cpu/simple-3-cycle-in-order-cpu/decode.hh"
#include "cpu/simple-3-cycle-in-order-cpu/execute.hh"
#include "cpu/simple-3-cycle-in-order-cpu/pfu.hh"
#include "mem/packet.hh"
#include "sim/ticked_object.hh"

namespace gem5
{
namespace simple3
{

class Simple3CycleCPU;

/**
 * The cycle event that drives the whole CPU.
 *
 * Pipeline inherits Ticked (not TickedObject) because the owning
 * Simple3CycleCPU is already a ClockedObject.  Ticked's machinery
 * schedules processClockEvent() every clockPeriod() ticks; our
 * evaluate() override is the gem5 equivalent of the Python model's
 * tick() function.
 *
 * evaluate() runs the six sub-operations atomically in the Python
 * order (E -> squash -> D->E -> FIFO->D -> F-issue).  The same-cycle
 * ordering constraints the Python model relied on are preserved by
 * sequencing the sub-ops within a single function call.  Async
 * events (icache responses arriving at non-cycle-edge ticks) are
 * staged by IcachePort::recvTimingResp into PFU::responseStaging
 * for the next cycle event to drain.
 */
class Pipeline : public Ticked
{
  public:
    explicit Pipeline(Simple3CycleCPU &cpu_);

    /** Ticked override — runs one cycle of the model. */
    void evaluate() override;

    /** Accessors for the owner (mostly for DPRINTF / stats). */
    PFU &getPfu() { return *pfu; }
    Decode &getDecode() { return *decode; }
    Execute &getExecute() { return *execute; }

    /** Phase-B helper: hand-fill the PFU FIFO with `count` 32-bit
     *  words at sequential addresses starting at `startAddr`,
     *  every word containing `wordValue`.  Used for the smoke test
     *  before icache is wired. */
    void prepopulateFifoForPhaseB(Addr startAddr, unsigned count,
                                  uint32_t wordValue);

    /** Phase-C helper: align the PFU and Decode tracking to the
     *  given entry PC.  Called from CPU startup after backing memory
     *  has been initialised. */
    void resetForPhaseC(Addr entryPc);

    /** Called from IcachePort::recvReqRetry — re-attempts the
     *  pending packet, if any. */
    void retrySendPendingFetch();

  private:
    Simple3CycleCPU &cpu;

    std::unique_ptr<PFU> pfu;
    std::unique_ptr<Decode> decode;
    std::unique_ptr<Execute> execute;

    /** Holds a fetch packet that sendTimingReq refused.  Re-issued
     *  either by recvReqRetry (immediately) or by the next cycle
     *  event (whichever happens first). */
    PacketPtr pendingRetryPacket = nullptr;

    /** Try once to send `pkt` over the icache port.  On success,
     *  bump in-flight counter and stats; on failure, stash in
     *  `pendingRetryPacket`.  Returns true on success. */
    bool trySendFetchPacket(PacketPtr pkt);

    /** Cortex-M4 E→D forwarding: if dSlot holds a direct branch and
     *  hasn't already been resolved, pre-run its execute() against
     *  the just-committed flags, mark it `earlyResolved`, and redirect
     *  F immediately if taken. */
    void earlyResolveBranchAtD(ThreadContext *tc);

    /** One-line-per-cycle trace compatible with the Python prototype. */
    void dumpLineTrace();
};

} // namespace simple3
} // namespace gem5

#endif // __CPU_SIMPLE_3_CYCLE_IN_ORDER_CPU_PIPELINE_HH__
