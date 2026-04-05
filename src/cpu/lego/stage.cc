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

#include "cpu/lego/stage.hh"

#include "cpu/lego/lego_cpu.hh"
#include "cpu/lego/stage_function.hh"
#include "cpu/lego/sub_stage.hh"

namespace gem5
{

Stage::Stage(const Params &params)
    : SimObject(params), _stageId(0), _cpu(nullptr)
{
    for (auto *sub : params.subStages) {
        sub->setStage(this);
        subStages.push_back(sub);
    }
}

void
Stage::setCPU(LegoCPU *cpu, unsigned stage_id)
{
    _cpu = cpu;
    _stageId = stage_id;

    // Set stageId on all functions in all substages
    for (auto *sub : subStages)
        for (auto *func : sub->getFunctions())
            func->setStageId(stage_id);
}

void
Stage::latchInputs()
{
    for (auto *sub : subStages)
        sub->latchInputs();
}

void
Stage::compute()
{
    for (auto *sub : subStages)
        sub->compute();
}

void
Stage::flush()
{
    for (auto *sub : subStages)
        sub->flush();
}

std::string
Stage::traceStatus() const
{
    std::string result;
    for (auto *sub : subStages) {
        for (auto *func : sub->getFunctions()) {
            std::string s = func->traceStatus();
            if (!s.empty()) {
                if (!result.empty())
                    result += ",";
                result += s;
            }
        }
    }
    return result.empty() ? "-" : result;
}

SimpleThread *
Stage::getThread(ThreadID tid)
{
    return _cpu->getThread(tid);
}

bool
Stage::isCurrentCycle(Tick tick) const
{
    return _cpu->isCurrentCycle(tick);
}

void
Stage::notifyUpdate()
{
    _cpu->notifyUpdate();
}

void
Stage::sendPacketToCPU(Packet *pkt)
{
    auto *ss = safe_cast<StageFunctionWithPort::FuncSenderState *>(
        pkt->senderState);
    ss->stage = this;

    _cpu->sendPacketToPort(pkt);
}

void
Stage::recvTimingResp(Packet *pkt)
{
    auto *ss = safe_cast<StageFunctionWithPort::FuncSenderState *>(
        pkt->senderState);

    ss->subStage->recvTimingResp(pkt);
}

void
Stage::sendTranslationToCPU(RequestPtr req,
                            BaseMMU::Translation *translation,
                            BaseMMU::Mode mode)
{
    _cpu->sendTranslationToMMU(req, translation, mode);
}

} // namespace gem5
