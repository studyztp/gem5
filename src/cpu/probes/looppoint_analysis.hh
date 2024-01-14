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

#ifndef __LOOPPOINT_ANALYSIS_HH__
#define __LOOPPOINT_ANALYSIS_HH__

#include <list>
#include <unordered_map>
#include <unordered_set>

#include "arch/generic/pcstate.hh"
#include "cpu/probes/pc_count_pair.hh"
#include "cpu/simple_thread.hh"
#include "debug/LooppointAnalysis.hh"
#include "params/LooppointAnalysis.hh"
#include "params/LooppointAnalysisManager.hh"
#include "sim/sim_exit.hh"

namespace gem5
{

class LooppointAnalysis : public ProbeListenerObject
{
  public:
    LooppointAnalysis(const LooppointAnalysisParams &p);

    void checkPc(const std::pair<SimpleThread*, StaticInstPtr> &instPair);

    virtual void regProbeListeners();
    void startListening();
    void stopListening();

    typedef ProbeListenerArg<LooppointAnalysis,
                                std::pair<SimpleThread*, StaticInstPtr>>
        LooppointAnalysisListener;

  private:

    LooppointAnalysisManager *lpamanager;
    AddrRange BBvalidAddrRange;
    AddrRange markerValidAddrRange;
    std::vector<AddrRange> BBexcludedAddrRanges;
    bool ifListeningFromStart;

    uint64_t BBInstCounter;

    std::unordered_map<Addr, uint64_t> localBBV;

    void updateLocalBBV(Addr pc);

  public:

    std::unordered_map<Addr, uint64_t>
    getLocalBBV() {
        return localBBV;
    };

    void
    clearLocalBBV() {
        localBBV.clear();
    }
};

class LooppointAnalysisManager : public SimObject
{
  public:
    LooppointAnalysisManager(const LooppointAnalysisManagerParams &p);
    void countPc(Addr pc);
    void updateBBV(Addr pc);

  private:
    std::unordered_map<Addr, uint64_t> counter;
    std::unordered_map<Addr, uint64_t> globalBBV;
    std::unordered_map<Addr, uint64_t> BBinstMap;

    uint64_t regionLength;
    uint64_t globalInstCounter;
    Addr mostRecentPc;

    std::unordered_set<Addr> backwardBranches;
    std::unordered_set<Addr> validPc;
    std::unordered_set<Addr> bbEnd;
    std::unordered_set<Addr> encounteredPc;

  public:

    bool
    ifPcBackwardBranch(Addr pc) {
        if (backwardBranches.find(pc) != backwardBranches.end()) {
            return true;
        } else {
            return false;
        }
    };

    bool
    ifPcValid(Addr pc) {
        if (validPc.find(pc) != validPc.end()) {
            return true;
        } else {
            return false;
        }
    };

    bool
    ifPcEncountered(Addr pc) {
        if (encounteredPc.find(pc) != encounteredPc.end()) {
            return true;
        } else {
            return false;
        }
    };

    bool
    ifPcBBEnd(Addr pc) {
        if (bbEnd.find(pc) != bbEnd.end()) {
            return true;
        } else {
            return false;
        }
    };

    void
    updateValidPc(Addr pc) {
        validPc.insert(pc);
    };

    void
    updatePcEncountered(Addr pc) {
        encounteredPc.insert(pc);
    };

    void
    updateBackwardBranches(Addr pc) {
        backwardBranches.insert(pc);
    };

    void
    updateBBEnd(Addr pc) {
        bbEnd.insert(pc);
    };

    void
    updateBBInstMap(Addr pc, uint64_t instCount) {
        if (BBinstMap.find(pc) == BBinstMap.end()) {
            BBinstMap.insert(std::make_pair(pc, instCount));
        }
    };

    std::unordered_map<Addr, uint64_t>
    getGlobalBBV() {
        return globalBBV;
    };

    void
    clearGlobalBBV() {
        globalBBV.clear();
    };

    uint64_t
    getGlobalInst() {
        return globalInstCounter;
    };

    void
    clearGlobalInst() {
        globalInstCounter = 0;
    };

    void
    increaseGlobalInst() {
        globalInstCounter++;
    };

    Addr
    getGlobalMostRecentPc() {
        return mostRecentPc;
    };

    std::unordered_set<Addr>
    getValidPc() {
        return validPc;
    };

    std::unordered_set<Addr>
    getBackwardBranches() {
        return backwardBranches;
    };

    std::unordered_set<Addr>
    getBBEnd() {
        return bbEnd;
    };

    std::unordered_set<Addr>
    getEncounteredPc() {
        return encounteredPc;
    };

    std::unordered_map<Addr, uint64_t>
    getBBInstMap() {
        return BBinstMap;
    };

    std::unordered_map<Addr, uint64_t>
    getCounter() {
        return counter;
    };

};

} // namespace gem5

#endif // __LOOPPOINT_ANALYSIS_HH__
