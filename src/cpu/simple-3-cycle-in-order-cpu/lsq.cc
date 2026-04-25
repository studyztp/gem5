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

#include "cpu/simple-3-cycle-in-order-cpu/lsq.hh"

#include <cstring>

#include "base/logging.hh"
#include "cpu/simple-3-cycle-in-order-cpu/simple_3cycle_cpu.hh"
#include "debug/Simple3CPULSQ.hh"

namespace gem5
{
namespace simple3
{

LSQ::LSQ(Simple3CycleCPU &cpu_) : cpu(cpu_)
{
}

Fault
LSQ::pushReadRequest(Addr paddr, unsigned size, Request::Flags flags)
{
    panic_if(_state != Idle,
             "LSQ::pushReadRequest while not Idle (state=%d)", _state);

    RequestPtr req = std::make_shared<Request>(
        paddr, size, flags, cpu.dataRequestorId());
    PacketPtr pkt = new Packet(req, MemCmd::ReadReq);
    pkt->allocate();
    DPRINTF(Simple3CPULSQ,
            "push read paddr=%#x size=%u\n", paddr, size);
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
        paddr, size, flags, cpu.dataRequestorId());
    PacketPtr pkt = new Packet(req, MemCmd::WriteReq);
    pkt->allocate();
    std::memcpy(pkt->getPtr<uint8_t>(), data, size);
    DPRINTF(Simple3CPULSQ,
            "push write paddr=%#x size=%u data[0]=%#x\n",
            paddr, size, data[0]);
    trySend(pkt);
    return NoFault;
}

void
LSQ::trySend(PacketPtr pkt)
{
    _state = Pending;
    _pkt = pkt;
    if (cpu.getDcachePort().sendTimingReq(pkt)) {
        _needsRetry = false;
        cpu.simple3Stats.dcacheRequestsIssued++;
    } else {
        // Park; recvReqRetry will call retrySend().
        _needsRetry = true;
        DPRINTF(Simple3CPULSQ, "send refused, parking for retry\n");
    }
}

void
LSQ::retrySend()
{
    if (!_needsRetry || !_pkt)
        return;
    DPRINTF(Simple3CPULSQ, "retrySend\n");
    if (cpu.getDcachePort().sendTimingReq(_pkt)) {
        _needsRetry = false;
        cpu.simple3Stats.dcacheRequestsIssued++;
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
    cpu.simple3Stats.dcacheResponsesReceived++;
    DPRINTF(Simple3CPULSQ,
            "complete paddr=%#x size=%u cmd=%s\n",
            pkt->getAddr(), pkt->getSize(), pkt->cmdString());
}

void
LSQ::release()
{
    panic_if(_state != Complete,
             "LSQ::release while not Complete (state=%d)", _state);
    delete _pkt;
    _pkt = nullptr;
    _state = Idle;
    DPRINTF(Simple3CPULSQ, "release\n");
}

} // namespace simple3
} // namespace gem5
