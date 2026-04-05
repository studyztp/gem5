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

#include "cpu/lego/sub_stage.hh"

#include "cpu/lego/stage.hh"
#include "cpu/lego/stage_function.hh"

namespace gem5
{

SubStage::SubStage(const Params &params)
    : SimObject(params), _stage(nullptr)
{
    for (auto *func : params.functions) {
        func->setSubStage(this);
        functions.push_back(func);
    }
}

void
SubStage::latchInputs()
{
    for (auto *func : functions)
        func->latchInputs();
}

void
SubStage::compute()
{
    for (auto *func : functions)
        func->compute();
}

void
SubStage::flush()
{
    for (auto *func : functions)
        func->flush();
}

SimpleThread *
SubStage::getThread(ThreadID tid)
{
    return _stage->getThread(tid);
}

bool
SubStage::isCurrentCycle(Tick tick) const
{
    return _stage->isCurrentCycle(tick);
}

void
SubStage::notifyUpdate()
{
    _stage->notifyUpdate();
}

void
SubStage::sendPacketToStage(Packet *pkt)
{
    auto *ss = safe_cast<StageFunctionWithPort::FuncSenderState *>(
        pkt->senderState);
    ss->subStage = this;

    _stage->sendPacketToCPU(pkt);
}

void
SubStage::sendTranslationToStage(RequestPtr req,
                                 BaseMMU::Translation *translation,
                                 BaseMMU::Mode mode)
{
    _stage->sendTranslationToCPU(req, translation, mode);
}

void
SubStage::recvTimingResp(Packet *pkt)
{
    // Peek at the sender state to find the function, but don't pop.
    // The function's recvTimingResp() will pop and delete it.
    auto *ss = safe_cast<StageFunctionWithPort::FuncSenderState *>(
        pkt->senderState);

    ss->func->recvTimingResp(pkt);
}

} // namespace gem5
