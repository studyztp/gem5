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

#ifndef __CPU_SIGNAL_3_STAGE_IN_ORDER_CPU_LSQ_HH__
#define __CPU_SIGNAL_3_STAGE_IN_ORDER_CPU_LSQ_HH__

#include "base/types.hh"
#include "cpu/signal-3-stage-in-order-cpu/signal.hh"
#include "mem/packet.hh"
#include "mem/request.hh"
#include "sim/faults.hh"

namespace gem5
{
namespace signal3
{

class SignalCPU;

/**
 * Single-slot Load/Store Queue, signal-driven variant.
 *
 * Mirrors the predecessor's three-state machine (Idle/Pending/Complete)
 * and adds a single Signal<bool> `_completionSignal` that toggles when
 * a response lands.  E subscribes to that signal so it re-fires
 * immediately when a response arrives — same-cycle if the response
 * arrived inside this cycle's window, next cycle otherwise.
 *
 * Cache-line splitting: gem5 cache models assert that any request fits
 * inside one cache block (BaseCache::satisfyRequest asserts
 * `offset + size <= blkSize`). The Cortex-M4 ISA allows 8-byte LDRD /
 * STRD at 4-byte-aligned addresses, which the AHB-Lite bus implements
 * as two sequential 32-bit transactions in real hardware. We model
 * that behaviour by splitting any request whose [paddr, paddr+size)
 * crosses a cache-line boundary into two sequential sub-requests,
 * issued back-to-back on the dcache port. The user-facing completion
 * signal pulses only after both halves arrive, with the combined
 * data assembled into `_pkt`.
 */
class LSQ
{
  public:
    enum State { Idle, Pending, Complete };

    LSQ(SignalGraph &g, SignalCPU &cpu_);

    State state() const { return _state; }
    bool idle() const { return _state == Idle; }
    bool pending() const { return _state == Pending; }
    bool complete() const { return _state == Complete; }

    /** Push a load. */
    Fault pushReadRequest(Addr paddr, unsigned size,
                          Request::Flags flags);
    /** Push a store. */
    Fault pushWriteRequest(const uint8_t *data, Addr paddr,
                           unsigned size, Request::Flags flags);

    /** Called from DcachePort::recvTimingResp when the response
     *  packet arrives.  Transitions to Complete and pulses the
     *  _completionSignal so subscribed stages (E) re-fire.
     *  For a split request, the first sub-packet's response triggers
     *  issuance of the second sub-packet; only the second response
     *  pulses the completion signal. */
    void completeRequest(PacketPtr pkt);

    /** Called from DcachePort::recvReqRetry to re-issue the parked
     *  request. */
    void retrySend();

    /** Called by E after consuming the response via completeAcc(). */
    void release();

    PacketPtr completedPacket() const { return _pkt; }
    bool needsRetry() const { return _needsRetry; }

    /** Output signal that fires when state moves to Complete. */
    Signal<uint64_t> completionSignal;

  private:
    SignalCPU &_cpu;
    State _state = Idle;
    /** User-facing packet: holds the original (combined) address,
     *  size, and assembled data. For non-split requests it doubles
     *  as the in-flight packet. */
    PacketPtr _pkt = nullptr;
    /** Currently in-flight packet on the dcache port. For a non-split
     *  request this equals `_pkt`; for a split request this is one of
     *  the sub-packets in `_subPkt`. Used to validate the response
     *  identity in `completeRequest` and to re-send on retry. */
    PacketPtr _inFlight = nullptr;
    bool _needsRetry = false;

    /** Split-request state. _isSplit is true when the original
     *  request crosses a cache-line boundary. _subPkt[0] / [1] are
     *  the two sub-packets (each contained in one cache line) issued
     *  sequentially. _subSize[0] / [1] are their sizes; their sum
     *  equals the original size. _subIdx is the index of the
     *  currently in-flight sub-packet (0 then 1). */
    bool _isSplit = false;
    PacketPtr _subPkt[2] = {nullptr, nullptr};
    unsigned _subSize[2] = {0, 0};
    int _subIdx = 0;

    /** Monotonic counter that ensures completionSignal::write detects
     *  a change (and re-queues subscribers) on each new response. */
    uint64_t _completionCounter = 0;

    void trySend(PacketPtr pkt);

    /** Issue `_subPkt[_subIdx]` (the next sub-packet of a split
     *  request). Mirrors the non-split fast path in pushRead/Write
     *  but is reused after the first sub-packet's response arrives.
     *  Increments _subIdx is the caller's responsibility. */
    void issueSub(PacketPtr subPkt);
};

} // namespace signal3
} // namespace gem5

#endif // __CPU_SIGNAL_3_STAGE_IN_ORDER_CPU_LSQ_HH__
