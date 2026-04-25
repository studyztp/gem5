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

#include "cpu/simple-3-cycle-in-order-cpu/decode.hh"

#include <cstring>

#include "arch/generic/decoder.hh"
#include "arch/generic/pcstate.hh"
#include "cpu/simple-3-cycle-in-order-cpu/pfu.hh"
#include "cpu/simple-3-cycle-in-order-cpu/simple_3cycle_cpu.hh"
#include "cpu/static_inst.hh"
#include "cpu/thread_context.hh"
#include "debug/Simple3CPUDecode.hh"

namespace gem5
{
namespace simple3
{

Decode::Decode(Simple3CycleCPU &cpu_)
    : cpu(cpu_), flushRemaining(0), _nextInstrAddrToDeliver(0)
{
}

void
Decode::reset()
{
    dSlot.reset();
    flushRemaining = Cycles(0);
    _nextInstrAddrToDeliver = 0;
    resetFetchTracking();
}

DecodedSlotEntry
Decode::popForExecute()
{
    DecodedSlotEntry e = *dSlot;
    dSlot.reset();
    return e;
}

void
Decode::stepOneInstr(PFU &pfu, ThreadContext *tc)
{
    // Already have an instruction queued?  Wait for E to take it.
    if (dSlot.has_value())
        return;

    InstDecoder *decoder = tc->getDecoderPtr();

    // Build a PCState whose instAddr() matches the next instruction
    // we want to deliver.  The decoder uses (pc.instAddr() - fetchPC)
    // to compute its byte offset within the fetch word, so the two
    // must be aligned with each other regardless of where the
    // thread's actual PC happens to be.
    std::unique_ptr<PCStateBase> instrPc(tc->pcState().clone());
    instrPc->set(_nextInstrAddrToDeliver);

    if (!decoder->instReady()) {
        if (decoder->needMoreBytes() || !_haveLoadedWord) {
            // Pull a fresh fetch word from the FIFO, skipping any
            // wrong-path leftovers below the per-instruction filter.
            std::optional<FetchWord> w;
            do {
                w = pfu.popFrontOfFifo();
            } while (w &&
                     w->addr + sizeof(uint32_t)
                         <= _nextInstrAddrToDeliver);
            if (!w)
                return;

            const size_t mb_size = decoder->moreBytesSize();
            DPRINTF(Simple3CPUDecode,
                    "feed addr=%#x data=%#08x to decoder (%lu bytes)\n",
                    w->addr, w->data, (unsigned long)mb_size);
            std::memcpy(decoder->moreBytesPtr(), &w->data,
                        std::min(mb_size, sizeof(uint32_t)));
            decoder->moreBytes(*instrPc, w->addr);
            _haveLoadedWord = true;
            _lastFetchPc = w->addr;
        } else {
            // Decoder still has bytes from the previous fetch — just
            // re-trigger process() by calling moreBytes with the
            // existing data and the new PC.  moreBytes recomputes
            // its internal offset from (pc - lastFetchPc).
            DPRINTF(Simple3CPUDecode,
                    "reuse previous word (lastFetchPc=%#x) for pc=%#x\n",
                    _lastFetchPc, _nextInstrAddrToDeliver);
            decoder->moreBytes(*instrPc, _lastFetchPc);
        }
    }

    if (!decoder->instReady())
        return;

    StaticInstPtr staticInst = decoder->decode(*instrPc);
    if (!staticInst)
        return;

    DecodedSlotEntry e;
    e.addr = _nextInstrAddrToDeliver;
    e.staticInst = staticInst;
    // The decoder mutated *instrPc so that pc=addr and npc=addr+size
    // (and any ISA-specific bits like Thumb mode are intact); freeze
    // a copy for Execute to restore before staticInst->execute().
    e.pcStateAtDecode.reset(instrPc->clone());
    // earlyResolved + resolvedNpc default to {false, 0}; Pipeline's
    // E→D forwarding step fills them when it pre-runs a direct
    // branch's execute() at D-time.

    DPRINTF(Simple3CPUDecode,
            "decoded inst @ pc=%#x size=%u isControl=%d isCondCtrl=%d "
            "isIndirectCtrl=%d\n",
            e.addr, staticInst->size(),
            (int)staticInst->isControl(),
            (int)staticInst->isCondCtrl(),
            (int)staticInst->isIndirectCtrl());

    _nextInstrAddrToDeliver = e.addr + staticInst->size();
    dSlot = e;
}

} // namespace simple3
} // namespace gem5
