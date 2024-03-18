/*
 * Copyright (c) 2023 The Regents of the University of California.
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

#include "cpu/probes/looppoint_analysis.hh"

namespace gem5
{

LooppointAnalysis::LooppointAnalysis(const LooppointAnalysisParams &p)
    : ProbeListenerObject(p),
    lpamanager(p.lpmanager),
    BBvalidAddrRange(p.BBValidAddrrange),
    markerValidAddrRange(p.markerValidAddrrange),
    ifListeningFromStart(p.ifStartListening),
    BBInstCounter(0)
{
    DPRINTF(LooppointAnalysis, "Start listening: %s\n",
                                ifListeningFromStart?"true":"false");
    for (int i = 0; i < p.BBexcludeAddrrange.size(); i++)
    {
        BBexcludedAddrRanges.push_back(
            AddrRange(
                p.BBexcludeAddrrange[i].start(),
                p.BBexcludeAddrrange[i].end()
            )
        );
        DPRINTF(
            LooppointAnalysis,
            "added  BBexcludedAddrRanges = (%li,%li)\n",
            BBexcludedAddrRanges.back().start(),
            BBexcludedAddrRanges.back().end()
        );
    }
    DPRINTF(LooppointAnalysis,
        "%i exclued addr ranges\n",
        BBexcludedAddrRanges.size());
    DPRINTF(LooppointAnalysis,
        "new  BBvalidAddrRange = (%li,%li)\n",
        BBvalidAddrRange.start(),
        BBvalidAddrRange.end());
    DPRINTF(LooppointAnalysis,
        "new  markerValidAddrRange = (%li,%li)\n",
        markerValidAddrRange.start(),
        markerValidAddrRange.end());
}

void
LooppointAnalysis::updateLocalBBV(Addr pc) {

    if (localBBV.find(pc) == localBBV.end())
    {
        localBBV.insert(std::make_pair(pc, 1));
    }
    else
    {
        localBBV.find(pc)->second++;
    }
}

void
LooppointAnalysis::checkPc(
    const std::pair<SimpleThread*, StaticInstPtr> &instPair
)
{
    SimpleThread* thread = instPair.first;
    const StaticInstPtr &inst = instPair.second;
    auto &pcstate =
                thread->getTC()->pcState().as<GenericISA::PCStateWithNext>();
    Addr pc = pcstate.pc();

    if (lpamanager->ifPcEncountered(pc))
    {
        if (lpamanager->ifPcValid(pc))
        {
            lpamanager->increaseGlobalInst();
            BBInstCounter ++;
        }
        else if (lpamanager->ifPcBBEnd(pc))
        {
            BBInstCounter ++;
            lpamanager->increaseGlobalInst();
            lpamanager->updateBBInstMap(pc, BBInstCounter);
            updateLocalBBV(pc);
            lpamanager->updateBBV(pc);
            BBInstCounter = 0;
            if (lpamanager->ifPcBackwardBranch(pc))
            {
                lpamanager->countPc(pc);
            }
        }
        return;
    }

    lpamanager->updatePcEncountered(pc);

    if (inst->isMicroop() && !inst->isLastMicroop())
    {
        return;
    }

    if (!thread->getIsaPtr()->inUserMode())
    {
        return;
    }

    if (BBvalidAddrRange.end() > 0 &&
        (pc < BBvalidAddrRange.start() || pc > BBvalidAddrRange.end()))
    {
        return;
    }

    if (BBexcludedAddrRanges.size()>0) {
        for (int i = 0; i < BBexcludedAddrRanges.size(); i++) {
            if (pc >= BBexcludedAddrRanges[i].start() &&
                pc <= BBexcludedAddrRanges[i].end()) {
                // if the current instruction is inside the Basic Block
                // excluded address range then ignore the current instruction
                return;
            }
        }
    }

    BBInstCounter ++;
    lpamanager->increaseGlobalInst();

    if (inst->isControl())
    {
        lpamanager->updateBBEnd(pc);
        lpamanager->updateBBInstMap(pc, BBInstCounter);
        updateLocalBBV(pc);
        lpamanager->updateBBV(pc);
        BBInstCounter = 0;

        if (markerValidAddrRange.end()>0
        && (pc < markerValidAddrRange.start()
                            || pc > markerValidAddrRange.end()))
        {
            return;
        }

        if (inst->isDirectCtrl()) {
            if (pcstate.npc() < pc) {
                lpamanager->updateBackwardBranches(pc, inst->disassemble(pc));
                lpamanager->countPc(pc);
            }
        }

    } else {
        lpamanager->updateValidPc(pc);
    }

}

void
LooppointAnalysis::regProbeListeners()
{
    if (ifListeningFromStart)
    {
        listeners.push_back(new LooppointAnalysisListener(this,
                            "Commit", &LooppointAnalysis::checkPc));
        DPRINTF(LooppointAnalysis, "Start listening to the RetiredInstsPC\n");
    }
}

void
LooppointAnalysis::startListening()
{
    if (listeners.size() == 0) {

        listeners.push_back(new LooppointAnalysisListener(this,
                            "Commit", &LooppointAnalysis::checkPc));
        DPRINTF(LooppointAnalysis, "Start listening to the RetiredInstsPC\n");
    }
}

void
LooppointAnalysis::stopListening()
{
    listeners.clear();
    DPRINTF(LooppointAnalysis, "Stop listening\n");
}

LooppointAnalysisManager::LooppointAnalysisManager(
    const LooppointAnalysisManagerParams &p
)
    : SimObject(p),
    regionLength(p.regionLen),
    globalInstCounter(0),
    mostRecentPc(0)
{
    DPRINTF(LooppointAnalysis, "regionLength = %i\n", regionLength);
}

void
LooppointAnalysisManager::countPc(Addr pc)
{
    if (counter.find(pc) == counter.end())
    {
        counter.insert(std::make_pair(pc, 1));
    }
    else
    {
        counter.find(pc)->second++;
    }

    mostRecentPc = pc;

    if (globalInstCounter >= regionLength)
    {
        exitSimLoopNow("simpoint starting point found");
    }
}

void
LooppointAnalysisManager::updateBBV(Addr pc)
{
    if (globalBBV.find(pc) == globalBBV.end())
    {
        globalBBV.insert(std::make_pair(pc, 1));
    }
    else
    {
        globalBBV.find(pc)->second++;
    }
}



} // namespace gem5
