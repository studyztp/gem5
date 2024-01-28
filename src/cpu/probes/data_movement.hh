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

#ifndef __CPU_PROBES_PC_COUNT_TRACKER_HH__
#define __CPU_PROBES_PC_COUNT_TRACKER_HH__

#include <unordered_map>
#include <unordered_set>

#include "cpu/simple_thread.hh"
#include "cpu/static_inst.hh"
#include "debug/DataMovementTracker.hh"
#include "mem/request.hh"
#include "params/DataMovementTracker.hh"
#include "sim/probe/probe.hh"
#include "sim/sim_exit.hh"

namespace gem5
{

class DataMovementTracker : public ProbeListenerObject
{
  public:
    DataMovementTracker(const DataMovementTrackerParams &p);

    typedef ProbeListenerArg<DataMovementTracker, RequestPtr>
                                                        ReadRequestListener;
    typedef ProbeListenerArg<DataMovementTracker, RequestPtr>
                                                        WriteRequestListener;
    typedef ProbeListenerArg<DataMovementTracker,
                      std::pair<StaticInstPtr,SimpleThread*>>PcListener;

    std::string addrToHex(Addr number) {
      std::stringstream ss;
      ss << std::hex << number;
      return ss.str();
    }

    std::string createPair(Addr addr1, Addr addr2) {
      std::stringstream ss;
      ss << addrToHex(addr1) << "&" << addrToHex(addr2);
      return ss.str();
    }

    uint64_t intervalLength;
    uint64_t intervalCount;
    uint64_t basicBlockInstCount;
    bool ifStartListening;

    std::unordered_map<std::string, uint64_t> readvAddrpAddrCount;
    std::unordered_map<std::string, uint64_t> writevAddrpAddrCount;
    std::unordered_map<std::string, uint64_t> readvAddrPcCount;
    std::unordered_map<std::string, uint64_t> writevAddrPcCount;
    std::unordered_map<Addr, Addr> curvAddrpAddr;
    std::unordered_map<Addr, uint64_t> vAddrMoveCount;

    std::unordered_map<Addr, uint64_t> basicBlockCount;
    std::unordered_map<Addr, uint64_t> basicBlockInstProfile;

    void getReadRequest(const RequestPtr &req);
    void getWriteRequest(const RequestPtr &req);
    void getPc(const std::pair<StaticInstPtr,SimpleThread*> &inst);

    virtual void regProbeListeners();
    void startListening();
    void stopListening();

    std::unordered_map<std::string, uint64_t>
    getReadvAddrpAddrCount() {
      return readvAddrpAddrCount;
    }

    void
    clearReadvAddrpAddrCount() {
      readvAddrpAddrCount.clear();
    }

    std::unordered_map<std::string, uint64_t>
    getWritevAddrpAddrCount() {
      return writevAddrpAddrCount;
    }

    void
    clearWritevAddrpAddrCount() {
      writevAddrpAddrCount.clear();
    }

    std::unordered_map<std::string, uint64_t>
    getReadvAddrPcCount() {
      return readvAddrPcCount;
    }

    void
    clearReadvAddrPcCount() {
      readvAddrPcCount.clear();
    }

    std::unordered_map<std::string, uint64_t>
    getWritevAddrPcCount() {
      return writevAddrPcCount;
    }

    void
    clearWritevAddrPcCount() {
      writevAddrPcCount.clear();
    }

    std::unordered_map<Addr, uint64_t>
    getvAddrMoveCount() {
      return vAddrMoveCount;
    }

    void
    clearvAddrMoveCount() {
      vAddrMoveCount.clear();
    }

    std::unordered_map<Addr, uint64_t>
    getBasicBlockCount() {
      return basicBlockCount;
    }

    void
    clearBasicBlockCount() {
      basicBlockCount.clear();
    }

    std::unordered_map<Addr, uint64_t>
    getBasicBlockInstProfile() {
      return basicBlockInstProfile;
    }

    uint64_t
    getIntervalCount() {
      return intervalCount;
    }

    void
    clearIntervalCount() {
      intervalCount = 0;
    }

};

} // namespace gem5

#endif // __CPU_PROBES_PC_COUNT_TRACKER_HH__
