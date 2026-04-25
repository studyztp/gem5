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

#ifndef __CPU_SIMPLE_3_CYCLE_IN_ORDER_CPU_LSQ_HH__
#define __CPU_SIMPLE_3_CYCLE_IN_ORDER_CPU_LSQ_HH__

#include <vector>

#include "base/types.hh"
#include "mem/packet.hh"
#include "mem/request.hh"
#include "sim/faults.hh"

namespace gem5
{
namespace simple3
{

class Simple3CycleCPU;

/**
 * Single-slot Load/Store Queue for Simple3CycleCPU.
 *
 * The CPU is in-order single-issue, so at most one outstanding
 * memory access exists at a time.  The LSQ has three states:
 *
 *   Idle      no request in flight; ExecContext may push a new one.
 *   Pending   request sent on dcache port; waiting for response.
 *   Complete  response received; Execute should call completeAcc()
 *             then release().
 *
 * Stores arrive as WriteReq packets and (on M-profile / acoherent
 * systems) generate WriteResp; we treat them the same as loads
 * (wait for the response, then release) so subsequent loads
 * observe ordering against earlier stores without extra logic.
 */
class LSQ
{
  public:
    enum State { Idle, Pending, Complete };

    explicit LSQ(Simple3CycleCPU &cpu_);

    State state() const { return _state; }
    bool idle() const { return _state == Idle; }
    bool pending() const { return _state == Pending; }
    bool complete() const { return _state == Complete; }

    /** Push a load: builds a ReadReq packet of `size` bytes at
     *  `paddr` and sends it on the dcache port.  On a successful
     *  send, transitions to Pending; on retry, parks the packet
     *  and sets `_needsRetry`. Returns NoFault. */
    Fault pushReadRequest(Addr paddr, unsigned size,
                          Request::Flags flags);

    /** Push a store: builds a WriteReq packet carrying `size` bytes
     *  copied from `data`, sends it.  Same Pending/retry behaviour
     *  as pushReadRequest. */
    Fault pushWriteRequest(const uint8_t *data, Addr paddr,
                           unsigned size, Request::Flags flags);

    /** Called from DcachePort::recvTimingResp when the response
     *  packet arrives.  Transitions to Complete. */
    void completeRequest(PacketPtr pkt);

    /** Called from DcachePort::recvReqRetry to re-issue the parked
     *  request. */
    void retrySend();

    /** Called by Execute after it has consumed the response via
     *  staticInst->completeAcc(packet, &xc, ...).  Frees the
     *  packet and returns the LSQ to Idle. */
    void release();

    /** Accessor for Execute's completeAcc() call. */
    PacketPtr completedPacket() const { return _pkt; }

    bool needsRetry() const { return _needsRetry; }

  private:
    Simple3CycleCPU &cpu;
    State _state = Idle;
    PacketPtr _pkt = nullptr;
    bool _needsRetry = false;

    /** Common send path used by both read and write pushes. */
    void trySend(PacketPtr pkt);
};

} // namespace simple3
} // namespace gem5

#endif // __CPU_SIMPLE_3_CYCLE_IN_ORDER_CPU_LSQ_HH__
