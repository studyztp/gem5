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

#include "cpu/signal-3-stage-in-order-cpu/lsq.hh"

#include <cstring>

#include "base/logging.hh"
#include "cpu/signal-3-stage-in-order-cpu/signal_cpu.hh"
#include "cpu/signal-3-stage-in-order-cpu/stats.hh"
#include "debug/Signal3CPUMem.hh"

namespace gem5
{
namespace signal3
{

LSQ::LSQ(SignalGraph &g, SignalCPU &cpu_)
    : completionSignal(g, "lsq_completion",
                       /*fromStage=*/nullptr, /*pulse=*/false),
      _cpu(cpu_)
{
}

Fault
LSQ::pushReadRequest(Addr paddr, unsigned size,
                     Request::Flags flags)
{
    panic_if(_state != Idle,
             "LSQ::pushReadRequest while not Idle (state=%d)", _state);
    RequestPtr req = std::make_shared<Request>(
        paddr, size, flags, _cpu.dataRequestorId());
    PacketPtr pkt = new Packet(req, MemCmd::ReadReq);
    pkt->allocate();
    DPRINTF(Signal3CPUMem,
            "lsq push read paddr=%#x size=%u\n", paddr, size);
    trySend(pkt);
    return NoFault;
}

Fault
LSQ::pushWriteRequest(const uint8_t *data, Addr paddr, unsigned size,
                      Request::Flags flags)
{
    panic_if(_state != Idle,
             "LSQ::pushWriteRequest while not Idle (state=%d)", _state);
    RequestPtr req = std::make_shared<Request>(
        paddr, size, flags, _cpu.dataRequestorId());
    PacketPtr pkt = new Packet(req, MemCmd::WriteReq);
    pkt->allocate();
    std::memcpy(pkt->getPtr<uint8_t>(), data, size);
    DPRINTF(Signal3CPUMem,
            "lsq push write paddr=%#x size=%u data[0]=%#x\n",
            paddr, size, data[0]);
    trySend(pkt);
    return NoFault;
}

void
LSQ::trySend(PacketPtr pkt)
{
    _state = Pending;
    _pkt = pkt;
    if (_cpu.getDcachePort().sendTimingReq(pkt)) {
        _needsRetry = false;
        _cpu.signalStats().dcacheRequestsIssued++;
    } else {
        _needsRetry = true;
        DPRINTF(Signal3CPUMem, "lsq dcache refused; parking for retry\n");
    }
}

void
LSQ::retrySend()
{
    if (!_needsRetry || !_pkt)
        return;
    DPRINTF(Signal3CPUMem, "lsq retrySend\n");
    if (_cpu.getDcachePort().sendTimingReq(_pkt)) {
        _needsRetry = false;
        _cpu.signalStats().dcacheRequestsIssued++;
    }
}

void
LSQ::completeRequest(PacketPtr pkt)
{
    panic_if(_state != Pending,
             "LSQ::completeRequest while not Pending (state=%d)",
             _state);
    panic_if(pkt != _pkt,
             "LSQ::completeRequest packet mismatch");
    _state = Complete;
    _cpu.signalStats().dcacheResponsesReceived++;
    DPRINTF(Signal3CPUMem,
            "lsq complete paddr=%#x size=%u cmd=%s\n",
            pkt->getAddr(), pkt->getSize(), pkt->cmdString());
    // Pulse the completion signal so any subscriber (Execute) re-fires.
    // Use a monotonic counter so back-to-back completions of the same
    // logical state still trigger the cascade through the equality
    // guard.
    completionSignal.write(++_completionCounter);
}

void
LSQ::release()
{
    panic_if(_state != Complete,
             "LSQ::release while not Complete (state=%d)", _state);
    delete _pkt;
    _pkt = nullptr;
    _state = Idle;
    DPRINTF(Signal3CPUMem, "lsq release -> Idle\n");
}

} // namespace signal3
} // namespace gem5
