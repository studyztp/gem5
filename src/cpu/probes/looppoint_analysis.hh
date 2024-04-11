/*
 * Copyright (c) 2024 The Regents of the University of California.
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

    /*
    * Check if the PC is a valid PC and process the PC for the looppoint
    * analysis.
    * For each unique PC, it only checks once, then it stores the PC in to the
    * corresponding data structure for faster checking and processing in the
    * future encounters of the same PC.
    *
    * A PC is considered valid if it satisfies all the following conditions:
    * 1. within the valid address range
    * 2. not in the excluded address range
    * 3. is a user-mode instruction
    *
    * If the PC is valid, it increment the global instruction counter in the
    * Looppoint manager, and increment the local instruction counter for basic
    * block instruction counting.
    *
    * If the PC is a branch instruction, it updates the counter for basic block
    * vector both locally in the current listener and globally in the Looppoint
    * manager. If it is the first time encountering the basic block, it updates
    * the number of instructions of the basic block.
    * It also reset the local instruction counter for basic block instruction.
    *
    * If the PC is a backward branch, it updates the backward branch set in the
    * Looppoint manager.
    * If it's the first time encountering the backward branch, it also updates
    * the disassembly of the backward branch instruction.
    *
    * @param instPair a pair of the thread object and the static instruction
    * of the commited instruction that is notified by the probe.
    *
    */
    void checkPc(const std::pair<SimpleThread*, StaticInstPtr> &instPair);

    virtual void regProbeListeners();
    /*
    * function that allows the simulator to start or stop listening to the
    * probes.
    */
    void startListening();
    void stopListening();

    typedef ProbeListenerArg<LooppointAnalysis,
                                std::pair<SimpleThread*, StaticInstPtr>>
        LooppointAnalysisListener;

  private:

    /*
    * The LooppointAnalysisManager object that stores the global information,
    * such as the global instruction counter, the global basic block vector,
    * the backward branch set, the valid PC set, the basic block end set, and
    * the encountered PC set.
    */
    LooppointAnalysisManager *lpamanager;
    /*
    * The address range of the valid PC.
    */
    AddrRange BBvalidAddrRange;

    /*
    * The address range of the marker PC.
    * We only want to use the backward branch instruction that is inside the
    * static code of the benchmark to be the marker PC.
    */
    AddrRange markerValidAddrRange;
    /*
    * The address range(s) of the excluded PC.
    */
    std::vector<AddrRange> BBexcludedAddrRanges;
    /*
    * If the listener should start listening from the beginning of the
    * simulation.
    */
    bool ifListeningFromStart;

    /*
    * The local instruction counter for basic block instruction counting.
    */
    uint64_t BBInstCounter;

    /*
    * The local basic block vector for each core.
    */
    std::unordered_map<Addr, uint64_t> localBBV;

    /*
    * Update the local basic block vector for the current core.
    *
    * @param pc the PC of the current instruction.
    */
    void updateLocalBBV(Addr pc);

  public:

    /*
    * Get the local basic block vector for the current core.
    */
    std::unordered_map<Addr, uint64_t>
    getLocalBBV() {
        return localBBV;
    };

    /*
    * Clear the local basic block vector for the current core.
    */
    void
    clearLocalBBV() {
        localBBV.clear();
    }
};

class LooppointAnalysisManager : public SimObject
{
  public:
    LooppointAnalysisManager(const LooppointAnalysisManagerParams &p);
    /*
    * Increment the global counter of the PC.
    *
    * @param pc the PC of the current backward branch instruction.
    */
    void updateBackwardBranchCounter(Addr pc);
    /*
    * Update the global basic block vector.
    *
    * @param pc the PC of the branch instruction of the basic block.
    */
    void updateBBV(Addr pc);

  private:
    std::unordered_map<Addr, uint64_t> backwardBranchCounter;
    std::unordered_map<Addr, uint64_t> globalBBV;
    std::unordered_map<Addr, uint64_t> BBinstMap;
    std::unordered_map<Addr, std::string> backBranchDisassembly;

    uint64_t regionLength;
    uint64_t globalInstCounter;
    Addr mostRecentPc;

    std::unordered_set<Addr> backwardBranches;
    std::unordered_set<Addr> validPc;
    std::unordered_set<Addr> bbEnd;
    std::unordered_set<Addr> encounteredPc;

  public:
    /*
    * Check if the PC is a backward branch.
    */
    bool
    ifPcBackwardBranch(Addr pc) {
        if (backwardBranches.find(pc) != backwardBranches.end()) {
            return true;
        } else {
            return false;
        }
    };

    /*
    * Check if the PC is a valid PC.
    */
    bool
    ifPcValid(Addr pc) {
        if (validPc.find(pc) != validPc.end()) {
            return true;
        } else {
            return false;
        }
    };

    /*
    * Check if the PC is encountered before.
    */
    bool
    ifPcEncountered(Addr pc) {
        if (encounteredPc.find(pc) != encounteredPc.end()) {
            return true;
        } else {
            return false;
        }
    };

    /*
    * Check if the PC is the end of the basic block.
    */
    bool
    ifPcBBEnd(Addr pc) {
        if (bbEnd.find(pc) != bbEnd.end()) {
            return true;
        } else {
            return false;
        }
    };

    /*
    * Update the valid PC set.
    */
    void
    updateValidPc(Addr pc) {
        validPc.insert(pc);
    };

    /*
    * Update the encountered PC set.
    */
    void
    updatePcEncountered(Addr pc) {
        encounteredPc.insert(pc);
    };

    /*
    * Update the backward branch set.
    */
    void
    updateBackwardBranches(Addr pc, std::string disassembly) {
        backwardBranches.insert(pc);
        backBranchDisassembly.insert(std::make_pair(pc, disassembly));
    };

    /*
    * Update the end of the basic block set.
    */
    void
    updateBBEnd(Addr pc) {
        bbEnd.insert(pc);
    };

    /*
    * Update the global instruction counter.
    */
    void
    updateBBInstMap(Addr pc, uint64_t instCount) {
        if (BBinstMap.find(pc) == BBinstMap.end()) {
            BBinstMap.insert(std::make_pair(pc, instCount));
        }
    };

    /*
    * Get the global basic block vector.
    */
    std::unordered_map<Addr, uint64_t>
    getGlobalBBV() {
        return globalBBV;
    };

    /*
    * Clear the global basic block vector.
    */
    void
    clearGlobalBBV() {
        globalBBV.clear();
    };

    /*
    * Get the global instruction counter.
    */
    uint64_t
    getGlobalInst() {
        return globalInstCounter;
    };

    /*
    * Clear the global instruction counter.
    */
    void
    clearGlobalInst() {
        globalInstCounter = 0;
    };

    /*
    * Increment the global instruction counter.
    */
    void
    increaseGlobalInst() {
        globalInstCounter++;
    };

    /*
    * Get the most recent encountered PC.
    */
    Addr
    getGlobalMostRecentPc() {
        return mostRecentPc;
    };

    /*
    * Clear the most recent encountered PC.
    */
    std::unordered_set<Addr>
    getValidPc() {
        return validPc;
    };

    /*
    * Get the backward branch set.
    */
    std::unordered_set<Addr>
    getBackwardBranches() {
        return backwardBranches;
    };

    /*
    * Get the end of the basic block set.
    */
    std::unordered_set<Addr>
    getBBEnd() {
        return bbEnd;
    };

    /*
    * Get the encountered PC set.
    */
    std::unordered_set<Addr>
    getEncounteredPc() {
        return encounteredPc;
    };

    /*
    * Get the basic block instruction counter map.
    */
    std::unordered_map<Addr, uint64_t>
    getBBInstMap() {
        return BBinstMap;
    };

    /*
    * Get the backward branch counter.
    */
    std::unordered_map<Addr, uint64_t>
    getCounter() {
        return backwardBranchCounter;
    };

    /*
    * Get the disassembly of the backward branch instruction.
    */
    std::unordered_map<Addr, std::string>
    getBackBranchDisassembly() {
        return backBranchDisassembly;
    };

};

} // namespace gem5

#endif // __LOOPPOINT_ANALYSIS_HH__
