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

#ifndef __CPU_LEGO_TYPE_HH__
#define __CPU_LEGO_TYPE_HH__

#include <memory>
#include <vector>

#include "base/types.hh"
#include "cpu/static_inst_fwd.hh"
#include "sim/faults.hh"

namespace gem5
{

/** Port type for packet-based communication. */
enum PortType
{
    ICACHE,
    DCACHE
};

/** Data produced by FetchAddressGen after MMU translation. */
struct FetchAddr
{
    InstSeqNum seqNum;
    Addr pc;
    Addr paddr;
    unsigned size;

    bool operator==(const FetchAddr &o) const
    { return seqNum == o.seqNum && pc == o.pc
             && paddr == o.paddr && size == o.size; }
};

/** Data produced by FetchMemResponse after icache returns. */
struct FetchLine
{
    InstSeqNum seqNum;
    Addr pc;
    Addr lineBaseAddr;
    std::vector<uint8_t> data;
    unsigned validBytes;

    bool operator==(const FetchLine &o) const
    {
        if (seqNum != o.seqNum || pc != o.pc
            || lineBaseAddr != o.lineBaseAddr
            || validBytes != o.validBytes)
            return false;
        if (data.size() != o.data.size())
            return false;
        for (size_t i = 0; i < data.size(); i++) {
            if (data[i] != o.data[i])
                return false;
        }
        return true;
    }
};

/** Data produced by InstructionDecode. */
struct DecodedInst
{
    InstSeqNum seqNum;
    StaticInstPtr staticInst;
    Addr pc;

    bool operator==(const DecodedInst &o) const
    { return seqNum == o.seqNum && pc == o.pc; }
};

/** Data produced by ALUExecute. */
struct ExecResult
{
    InstSeqNum seqNum;
    StaticInstPtr staticInst;
    Addr pc;
    Fault fault;

    bool operator==(const ExecResult &o) const
    { return seqNum == o.seqNum && pc == o.pc; }
};

/** Control signal from BranchResolve / PCUpdate back to fetch. */
struct Redirect
{
    InstSeqNum seqNum;
    Addr target;
    bool valid;

    bool operator==(const Redirect &o) const
    { return seqNum == o.seqNum && target == o.target
             && valid == o.valid; }
};

} // namespace gem5

#endif // __CPU_LEGO_TYPE_HH__
