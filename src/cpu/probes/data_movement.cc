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

#include "cpu/probes/data_movement.hh"

namespace gem5 {
DataMovementTracker::DataMovementTracker(const DataMovementTrackerParams &p)
    : ProbeListenerObject(p),
    intervalLength(p.interval_length),
    intervalCount(0),
    basicBlockInstCount(0),
    ifStartListening(p.ifStart)
{
    DPRINTF(DataMovementTracker,
        "Got to DataMovementTracker constructor.\n");
    DPRINTF(DataMovementTracker,
        "intervalLength: %ld\n", intervalLength);
    DPRINTF(DataMovementTracker,
        "ifStartListening: %s\n", (ifStartListening? "true" : "false"));
}

void
DataMovementTracker::getReadRequest(const RequestPtr &req)
{
    std::pair<Addr, Addr> pair = req->getVPNnPPN();
    Addr vAddr = pair.first;
    Addr pAddr = pair.second;
    Addr pc = req->getPC();

    DPRINTF(DataMovementTracker,
        "Got to getReadRequest.\n");

    if (readvAddrpAddrCount.find(createPair(vAddr, pAddr))
                                                == readvAddrpAddrCount.end())
    {
        readvAddrpAddrCount.insert(std::make_pair<std::string, uint64_t>
                                                (createPair(vAddr, pAddr), 1));
    }
    else
    {
        readvAddrpAddrCount.find(createPair(vAddr, pAddr))->second++;
    }

    if (readvAddrPcCount.find(createPair(vAddr, pc))
                                                == readvAddrPcCount.end())
    {
        readvAddrPcCount.insert(std::make_pair<std::string, uint64_t>
                                                (createPair(vAddr, pc), 1));
    }
    else
    {
        readvAddrPcCount.find(createPair(vAddr, pc))->second++;
    }

    // if (curvAddrpAddr.find(vAddr) == curvAddrpAddr.end()) {
    //     curvAddrpAddr.insert(std::make_pair<Addr, Addr>(req->getVaddr(),
    //                                                       req->getPaddr()));
    // }
    // else {
    //     if (curvAddrpAddr.find(vAddr)->second != pAddr) {
    //         curvAddrpAddr.find(vAddr)->second = pAddr;
    //         if (vAddrMoveCount.find(vAddr) == vAddrMoveCount.end()) {
    //             vAddrMoveCount.insert(
    //                                 std::make_pair<Addr, uint64_t>(
    //                                     req->getVaddr(), 0));
    //         }
    //         else {
    //             vAddrMoveCount.find(vAddr)->second++;
    //         }
    //     }
    // }
}

void
DataMovementTracker::getWriteRequest(const RequestPtr &req)
{
    std::pair<Addr, Addr> pair = req->getVPNnPPN();
    Addr vAddr = pair.first;
    Addr pAddr = pair.second;
    Addr pc = req->getPC();

    DPRINTF(DataMovementTracker,
        "Got to getWriteRequest.\n");

    if (writevAddrpAddrCount.find(createPair(vAddr, pAddr))
                                                == writevAddrpAddrCount.end())
    {
        writevAddrpAddrCount.insert(std::make_pair<std::string, uint64_t>
                                                (createPair(vAddr, pAddr), 1));
    }
    else
    {
        writevAddrpAddrCount.find(createPair(vAddr, pAddr))->second++;
    }

    if (writevAddrPcCount.find(createPair(vAddr, pc))
                                                == writevAddrPcCount.end())
    {
        writevAddrPcCount.insert(std::make_pair<std::string, uint64_t>
                                                (createPair(vAddr, pc), 1));
    }
    else
    {
        writevAddrPcCount.find(createPair(vAddr, pc))->second++;
    }

    // if (curvAddrpAddr.find(vAddr) == curvAddrpAddr.end()) {
    //     curvAddrpAddr.insert(std::make_pair<Addr, Addr>(
    //                                 req->getVaddr(), req->getPaddr()));
    // }
    // else {
    //     if (curvAddrpAddr.find(vAddr)->second != pAddr) {
    //         curvAddrpAddr.find(vAddr)->second = pAddr;
    //         if (vAddrMoveCount.find(vAddr) == vAddrMoveCount.end()) {
    //             vAddrMoveCount.insert(
    //                                 std::make_pair<Addr, uint64_t>(
    //                                                 req->getVaddr(), 0));
    //         }
    //         else {
    //             vAddrMoveCount.find(vAddr)->second++;
    //         }
    //     }
    // }
}

void
DataMovementTracker::getPc(const std::pair<Addr, bool> &inst)
{
    Addr pc = inst.first;
    bool isControl = inst.second;

    basicBlockInstCount ++;
    intervalCount ++;

    if (isControl) {
        if (basicBlockCount.find(pc) == basicBlockCount.end()) {
            basicBlockCount.insert(std::make_pair<Addr, uint64_t>(
                                                std::forward<Addr>(pc), 1));
        }
        else {
            basicBlockCount.find(pc)->second++;
        }
        if (basicBlockInstProfile.find(pc) == basicBlockInstProfile.end()) {
            basicBlockInstProfile.insert(
                                    std::make_pair<Addr, uint64_t>(
        std::forward<Addr>(pc),std::forward<uint64_t>(basicBlockInstCount)));
        }

        DPRINTF(DataMovementTracker,
            "intervalCount: %ld\n", intervalCount);


        basicBlockInstCount = 0;
        if (intervalCount >= intervalLength)
        {
            exitSimLoopNow("simpoint starting point found");
        }
    }
}

void
DataMovementTracker::regProbeListeners()
{
    if (ifStartListening) {
        DPRINTF(DataMovementTracker,
            "start listening.\n");
        listeners.push_back(new ReadRequestListener(this, "ReadRequestProbe",
                    &DataMovementTracker::getReadRequest));
        listeners.push_back(new WriteRequestListener(this, "WriteRequestProbe",
                    &DataMovementTracker::getWriteRequest));
        listeners.push_back(new PcListener(this, "PcProbe",
                    &DataMovementTracker::getPc));
    }
}

void
DataMovementTracker::startListening()
{
    if (listeners.size() <= 0)
    {
        DPRINTF(DataMovementTracker,
            "start listening.\n");
        listeners.push_back(new ReadRequestListener(this, "ReadRequestProbe",
                    &DataMovementTracker::getReadRequest));
        listeners.push_back(new WriteRequestListener(this, "WriteRequestProbe",
                    &DataMovementTracker::getWriteRequest));
        listeners.push_back(new PcListener(this, "PcProbe",
                    &DataMovementTracker::getPc));
    }
}

void
DataMovementTracker::stopListening()
{

    DPRINTF(DataMovementTracker,
        "stop listening.\n");
    listeners.clear();
}


}   // namespace gem5
