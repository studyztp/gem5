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

namespace
{

// Return the number of bytes in [paddr, paddr+size) that fall inside
// the first cache line. Equals `size` when the access fits in one
// line; otherwise (line_size - (paddr % line_size)).
inline unsigned
firstLineSize(Addr paddr, unsigned size, Addr lineSize)
{
    const Addr lineMask = lineSize - 1;
    const Addr lineEnd = (paddr & ~lineMask) + lineSize;
    const Addr endAddr = paddr + size;
    if (endAddr <= lineEnd)
        return size;
    return lineEnd - paddr;
}

} // anonymous namespace

Fault
LSQ::pushReadRequest(Addr paddr, unsigned size,
                     Request::Flags flags)
{
    panic_if(_state != Idle,
             "LSQ::pushReadRequest while not Idle (state=%d)", _state);

    const Addr lineSize = _cpu.cacheLineSize();
    const unsigned firstSize = firstLineSize(paddr, size, lineSize);

    if (firstSize == size) {
        // Fits in one cache line — single-request fast path.
        _isSplit = false;
        RequestPtr req = std::make_shared<Request>(
            paddr, size, flags, _cpu.dataRequestorId());
        PacketPtr pkt = new Packet(req, MemCmd::ReadReq);
        pkt->allocate();
        _pkt = pkt;
        DPRINTF(Signal3CPUMem,
                "lsq push read paddr=%#x size=%u\n", paddr, size);
        trySend(pkt);
        return NoFault;
    }

    // Crosses a cache-line boundary — split into two sub-requests.
    // Mirrors the Cortex-M4 AHB-Lite implementation of 8-byte LDRD
    // (two sequential 32-bit transactions on real silicon).
    _isSplit = true;
    _subIdx = 0;
    const unsigned secondSize = size - firstSize;
    _subSize[0] = firstSize;
    _subSize[1] = secondSize;

    // User-facing packet — combined view, no in-flight send.
    RequestPtr userReq = std::make_shared<Request>(
        paddr, size, flags, _cpu.dataRequestorId());
    _pkt = new Packet(userReq, MemCmd::ReadReq);
    _pkt->allocate();

    // Sub-packet 0: [paddr, paddr + firstSize) — head of first line.
    RequestPtr req0 = std::make_shared<Request>(
        paddr, firstSize, flags, _cpu.dataRequestorId());
    _subPkt[0] = new Packet(req0, MemCmd::ReadReq);
    _subPkt[0]->allocate();

    // Sub-packet 1: [paddr + firstSize, paddr + size) — tail in next line.
    RequestPtr req1 = std::make_shared<Request>(
        paddr + firstSize, secondSize, flags, _cpu.dataRequestorId());
    _subPkt[1] = new Packet(req1, MemCmd::ReadReq);
    _subPkt[1]->allocate();

    DPRINTF(Signal3CPUMem,
            "lsq push read SPLIT paddr=%#x size=%u → "
            "[%#x size=%u][%#x size=%u] (lineSize=%llu)\n",
            paddr, size,
            paddr, firstSize, paddr + firstSize, secondSize,
            (unsigned long long)lineSize);
    trySend(_subPkt[0]);
    return NoFault;
}

Fault
LSQ::pushWriteRequest(const uint8_t *data, Addr paddr, unsigned size,
                      Request::Flags flags)
{
    panic_if(_state != Idle,
             "LSQ::pushWriteRequest while not Idle (state=%d)", _state);

    const Addr lineSize = _cpu.cacheLineSize();
    const unsigned firstSize = firstLineSize(paddr, size, lineSize);

    if (firstSize == size) {
        // Fits in one cache line — single-request fast path.
        _isSplit = false;
        RequestPtr req = std::make_shared<Request>(
            paddr, size, flags, _cpu.dataRequestorId());
        PacketPtr pkt = new Packet(req, MemCmd::WriteReq);
        pkt->allocate();
        std::memcpy(pkt->getPtr<uint8_t>(), data, size);
        _pkt = pkt;
        DPRINTF(Signal3CPUMem,
                "lsq push write paddr=%#x size=%u data[0]=%#x\n",
                paddr, size, data[0]);
        trySend(pkt);
        return NoFault;
    }

    // Crosses a cache-line boundary — split into two sub-requests.
    _isSplit = true;
    _subIdx = 0;
    const unsigned secondSize = size - firstSize;
    _subSize[0] = firstSize;
    _subSize[1] = secondSize;

    // User-facing packet — combined view, no in-flight send. Keeps the
    // full data buffer so any future reader of _pkt sees the entire
    // write payload at the original [paddr, paddr+size).
    RequestPtr userReq = std::make_shared<Request>(
        paddr, size, flags, _cpu.dataRequestorId());
    _pkt = new Packet(userReq, MemCmd::WriteReq);
    _pkt->allocate();
    std::memcpy(_pkt->getPtr<uint8_t>(), data, size);

    // Sub-packet 0: first-line slice of the write payload.
    RequestPtr req0 = std::make_shared<Request>(
        paddr, firstSize, flags, _cpu.dataRequestorId());
    _subPkt[0] = new Packet(req0, MemCmd::WriteReq);
    _subPkt[0]->allocate();
    std::memcpy(_subPkt[0]->getPtr<uint8_t>(), data, firstSize);

    // Sub-packet 1: second-line slice of the write payload.
    RequestPtr req1 = std::make_shared<Request>(
        paddr + firstSize, secondSize, flags, _cpu.dataRequestorId());
    _subPkt[1] = new Packet(req1, MemCmd::WriteReq);
    _subPkt[1]->allocate();
    std::memcpy(_subPkt[1]->getPtr<uint8_t>(),
                data + firstSize, secondSize);

    DPRINTF(Signal3CPUMem,
            "lsq push write SPLIT paddr=%#x size=%u → "
            "[%#x size=%u][%#x size=%u] (lineSize=%llu)\n",
            paddr, size,
            paddr, firstSize, paddr + firstSize, secondSize,
            (unsigned long long)lineSize);
    trySend(_subPkt[0]);
    return NoFault;
}

void
LSQ::trySend(PacketPtr pkt)
{
    _state = Pending;
    _inFlight = pkt;
    if (_cpu.getDcachePort().sendTimingReq(pkt)) {
        _needsRetry = false;
        _cpu.signalStats().dcacheRequestsIssued++;
    } else {
        _needsRetry = true;
        DPRINTF(Signal3CPUMem, "lsq dcache refused; parking for retry\n");
    }
}

void
LSQ::issueSub(PacketPtr subPkt)
{
    // Same effect as trySend(), but called from completeRequest's
    // mid-pipeline path where _state is already Pending and we want
    // to keep it that way (E should not see a transient Idle).
    _inFlight = subPkt;
    if (_cpu.getDcachePort().sendTimingReq(subPkt)) {
        _needsRetry = false;
        _cpu.signalStats().dcacheRequestsIssued++;
    } else {
        _needsRetry = true;
        DPRINTF(Signal3CPUMem,
                "lsq dcache refused sub-pkt; parking for retry\n");
    }
}

void
LSQ::retrySend()
{
    if (!_needsRetry || !_inFlight)
        return;
    DPRINTF(Signal3CPUMem, "lsq retrySend\n");
    if (_cpu.getDcachePort().sendTimingReq(_inFlight)) {
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
    panic_if(pkt != _inFlight,
             "LSQ::completeRequest packet mismatch");
    _cpu.signalStats().dcacheResponsesReceived++;

    if (_isSplit) {
        // Reassemble: copy this sub-packet's response data into the
        // user-facing packet's buffer at the correct offset. Writes
        // don't need data copy here (the response data has no useful
        // payload for a write), but the offset advance is the same.
        const unsigned offset = (_subIdx == 0) ? 0 : _subSize[0];
        if (pkt->isRead()) {
            std::memcpy(_pkt->getPtr<uint8_t>() + offset,
                        pkt->getPtr<uint8_t>(),
                        _subSize[_subIdx]);
        }

        if (_subIdx == 0) {
            // First half done — fire off the second sub-packet without
            // touching _state (stays Pending) or pulsing the completion
            // signal (E should not see this partial completion).
            _subIdx = 1;
            DPRINTF(Signal3CPUMem,
                    "lsq SPLIT half 1/2 complete; issuing 2/2 "
                    "paddr=%#x size=%u\n",
                    _subPkt[1]->getAddr(), _subSize[1]);
            issueSub(_subPkt[1]);
            return;
        }

        // _subIdx == 1: second half done — assembly complete.
        DPRINTF(Signal3CPUMem,
                "lsq SPLIT half 2/2 complete; user pkt assembled "
                "paddr=%#x size=%u\n",
                _pkt->getAddr(), _pkt->getSize());
    }

    _state = Complete;
    DPRINTF(Signal3CPUMem,
            "lsq complete paddr=%#x size=%u cmd=%s\n",
            _pkt->getAddr(), _pkt->getSize(), _pkt->cmdString());
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
    _inFlight = nullptr;
    if (_isSplit) {
        delete _subPkt[0];
        delete _subPkt[1];
        _subPkt[0] = _subPkt[1] = nullptr;
        _subSize[0] = _subSize[1] = 0;
        _subIdx = 0;
        _isSplit = false;
    }
    _state = Idle;
    DPRINTF(Signal3CPUMem, "lsq release -> Idle\n");
}

} // namespace signal3
} // namespace gem5
