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

#include "cpu/lego/stage_function.hh"

#include "cpu/lego/lego_cpu.hh"
#include "cpu/lego/stage.hh"
#include "cpu/lego/sub_stage.hh"

namespace gem5
{

// --------- StageFunction ---------

StageFunction::StageFunction(const Params &params)
    : SimObject(params), funcName(params.funcName)
{}

void
StageFunction::setStageId(unsigned id)
{
    stageId = id;
    // Update all registered ports with the new stage ID
    for (auto &[name, port] : portMap)
        port->setStageId(stageId);
}

bool
StageFunction::isCurrentCycle(Tick tick) const
{
    return _subStage->isCurrentCycle(tick);
}

void
StageFunction::notifyUpdate()
{
    if (_subStage)
        _subStage->notifyUpdate();
}

void
StageFunction::registerPort(const std::string &name, PortBase *port)
{
    portMap[name] = port;
}

PortBase *
StageFunction::findPort(const std::string &name) const
{
    auto it = portMap.find(name);
    if (it != portMap.end())
        return it->second;
    return nullptr;
}

void
StageFunction::wireOutput(const std::string &portName,
                          StageFunction *consumer)
{
    PortBase *outPort = findPort(portName);
    fatal_if(!outPort, "wireOutput: no port '%s' on %s\n",
             portName, name());
    fatal_if(!outPort->isOutput(), "wireOutput: '%s' on %s is not output\n",
             portName, name());

    PortBase *inPort = consumer->findPort(portName);
    fatal_if(!inPort, "wireOutput: no matching input '%s' on %s\n",
             portName, consumer->name());

    outPort->connectTo(inPort);
}

void
StageFunction::wireInput(const std::string &portName,
                         StageFunction *producer)
{
    PortBase *inPort = findPort(portName);
    fatal_if(!inPort, "wireInput: no port '%s' on %s\n",
             portName, name());
    fatal_if(inPort->isOutput(), "wireInput: '%s' on %s is not input\n",
             portName, name());

    PortBase *outPort = producer->findPort(portName);
    fatal_if(!outPort, "wireInput: no matching output '%s' on %s\n",
             portName, producer->name());

    outPort->connectTo(inPort);
}

// --------- StageFunctionTranslation ---------

StageFunctionTranslation::StageFunctionTranslation(const Params &params)
    : StageFunction(params), translationReq(nullptr)
{}

} // namespace gem5
