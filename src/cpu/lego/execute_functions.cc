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

#include "cpu/lego/execute_functions.hh"

#include "arch/generic/decoder.hh"
#include "cpu/lego/exec_context.hh"
#include "cpu/lego/lego_cpu.hh"
#include "cpu/lego/stage.hh"
#include "cpu/lego/sub_stage.hh"
#include "cpu/static_inst.hh"
#include "debug/LegoCPU.hh"
#include "debug/LegoCPUFunc.hh"

namespace gem5
{

// --------- InstructionDecode ---------

InstructionDecode::InstructionDecode(const Params &params)
    : StageFunction(params),
      fetchLineIn("fetchLine", 0, this),
      decodedInstOut("decodedInst", 0),
      lastDecodedSeqNum(0)
{
    registerPort("fetchLine", &fetchLineIn);
    registerPort("decodedInst", &decodedInstOut);
}

void
InstructionDecode::latchInputs()
{
    // Copy cross-stage input at cycle boundary
    if (fetchLineIn.hasData() &&
        fetchLineIn.getWriterStageId() != getStageId()) {
        const FetchLine &src = fetchLineIn.read();
        DPRINTF(LegoCPUFunc, "InstructionDecode::latchInputs() "
                "latching seq=%d pc=0x%x base=0x%x size=%d\n",
                src.seqNum, src.pc, src.lineBaseAddr,
                src.data.size());
        localFetchLine = src;
        localFetchLineValid = true;
    } else {
        DPRINTF(LegoCPUFunc, "InstructionDecode::latchInputs() "
                "no cross-stage data (hasData=%d writerStage=%d "
                "myStage=%d)\n",
                fetchLineIn.hasData(),
                fetchLineIn.getWriterStageId(),
                getStageId());
    }
}

void
InstructionDecode::compute()
{
    // Same-stage: update local copy immediately
    if (fetchLineIn.hasData() &&
        fetchLineIn.getWriterStageId() == getStageId()) {
        localFetchLine = fetchLineIn.read();
        localFetchLineValid = true;
        DPRINTF(LegoCPUFunc, "InstructionDecode::compute() "
                "same-stage update seq=%d\n",
                localFetchLine.seqNum);
    }

    DPRINTF(LegoCPUFunc, "InstructionDecode::compute() "
            "localValid=%d localSeq=%d lastDecoded=%d\n",
            localFetchLineValid,
            localFetchLineValid ? localFetchLine.seqNum : 0,
            lastDecodedSeqNum);

    if (!localFetchLineValid)
        return;

    const FetchLine &line = localFetchLine;

    if (line.seqNum == lastDecodedSeqNum)
        return;

    lastDecodedSeqNum = line.seqNum;

    // Get the decoder from the thread
    ThreadContext *tc = _subStage->getThread(0)->getTC();

    // Feed bytes to the decoder
    auto *decoder = tc->getDecoderPtr();

    // Use the PC from the fetch line, not ThreadContext
    // (TC may have been advanced by PCUpdate already)
    Addr pcAddr = line.pc;
    DPRINTF(LegoCPUFunc, "InstructionDecode: decoding seq=%d "
            "pc=0x%x lineBase=0x%x lineSize=%d\n",
            line.seqNum, pcAddr, line.lineBaseAddr,
            line.data.size());
    Addr offset = pcAddr - line.lineBaseAddr;

    fatal_if(offset + decoder->moreBytesSize() > line.data.size(),
             "InstructionDecode: PC 0x%x offset %d exceeds line "
             "data size %d\n", pcAddr, offset, line.data.size());

    // Copy the right bytes at the PC offset
    std::memcpy(decoder->moreBytesPtr(),
                line.data.data() + offset,
                decoder->moreBytesSize());

    decoder->moreBytes(tc->pcState(), line.lineBaseAddr + offset);

    StaticInstPtr si = nullptr;
    if (decoder->instReady()) {
        auto pc = tc->pcState().clone();
        si = decoder->decode(*pc);
    }

    DecodedInst result;
    result.seqNum = line.seqNum;
    result.staticInst = si;
    result.pc = line.pc;

    decodedInstOut.write(result);

    DPRINTF(LegoCPUFunc, "InstructionDecode: seq=%d pc=0x%x %s\n",
            result.seqNum, result.pc,
            si ? si->disassemble(result.pc) : "???");
}

void
InstructionDecode::flush()
{
    lastDecodedSeqNum = 0;
    decodedInstOut.clear();
}

std::string
InstructionDecode::traceStatus() const
{
    if (decodedInstOut.hasData()) {
        const auto &inst = decodedInstOut.read();
        if (inst.staticInst)
            return csprintf("D:%s",
                inst.staticInst->disassemble(inst.pc));
        return csprintf("D:0x%x", inst.pc);
    }
    return "";
}

// --------- ALUExecute ---------

ALUExecute::ALUExecute(const Params &params)
    : StageFunction(params),
      decodedInstIn("decodedInst", 0, this),
      execResultOut("execResult", 0),
      lastExecutedSeqNum(0)
{
    registerPort("decodedInst", &decodedInstIn);
    registerPort("execResult", &execResultOut);
}

void
ALUExecute::compute()
{
    if (!decodedInstIn.hasData())
        return;

    const DecodedInst &inst = decodedInstIn.read();

    if (inst.seqNum == lastExecutedSeqNum)
        return;

    if (!inst.staticInst)
        return;

    lastExecutedSeqNum = inst.seqNum;

    SimpleThread *thread = _subStage->getThread(0);
    BaseCPU *cpu = _subStage->stage()->cpu();

    LegoExecContext xc(*cpu, *thread);
    Fault fault = inst.staticInst->execute(&xc, nullptr);

    ExecResult result;
    result.seqNum = inst.seqNum;
    result.staticInst = inst.staticInst;
    result.pc = inst.pc;
    result.fault = fault;
    result.branchTaken = false;  // PCUpdate detects this via advancePC

    execResultOut.write(result);

    DPRINTF(LegoCPUFunc, "ALUExecute: seq=%d pc=0x%x fault=%s\n",
            inst.seqNum, inst.pc,
            fault == NoFault ? "none" : fault->name());
}

void
ALUExecute::flush()
{
    lastExecutedSeqNum = 0;
    execResultOut.clear();
}

std::string
ALUExecute::traceStatus() const
{
    if (execResultOut.hasData())
        return csprintf("X:seq%d", execResultOut.read().seqNum);
    return "";
}

// --------- PCUpdate ---------

PCUpdate::PCUpdate(const Params &params)
    : StageFunction(params),
      execResultIn("execResult", 0, this),
      redirectOut("redirect", 0),
      lastUpdatedSeqNum(0)
{
    registerPort("execResult", &execResultIn);
    registerPort("redirect", &redirectOut);
}

void
PCUpdate::compute()
{
    if (!execResultIn.hasData())
        return;

    const ExecResult &result = execResultIn.read();

    if (result.seqNum == lastUpdatedSeqNum)
        return;

    lastUpdatedSeqNum = result.seqNum;

    ThreadContext *tc = _subStage->getThread(0)->getTC();

    // Save PC before advancing
    Addr pcBefore = tc->pcState().instAddr();

    // advancePC handles both sequential and branch cases.
    // For conditional branches, it checks condition flags
    // (set by execute) and goes to target or falls through.
    auto newPC = tc->pcState().clone();
    result.staticInst->advancePC(*newPC);
    tc->pcState(*newPC);

    Addr pcAfter = newPC->instAddr();

    // Sequential = PC advanced by instruction size (e.g., +4)
    // Branch taken = PC jumped somewhere else
    Addr expectedNext = pcBefore + result.staticInst->size();
    bool branchTaken = (pcAfter != expectedNext);

    Redirect redir;
    redir.seqNum = result.seqNum;
    redir.target = pcAfter;
    redir.valid = branchTaken;

    if (branchTaken) {
        DPRINTF(LegoCPUFunc, "PCUpdate: branch taken to 0x%x "
                "(was 0x%x, expected next 0x%x)\n",
                pcAfter, pcBefore, expectedNext);
    } else {
        DPRINTF(LegoCPUFunc, "PCUpdate: sequential 0x%x -> 0x%x\n",
                pcBefore, pcAfter);
    }

    redirectOut.write(redir);
}

void
PCUpdate::flush()
{
    lastUpdatedSeqNum = 0;
    redirectOut.clear();
}

std::string
PCUpdate::traceStatus() const
{
    if (redirectOut.hasData()) {
        const auto &r = redirectOut.read();
        if (r.valid)
            return csprintf("PC:redir->0x%x", r.target);
        return csprintf("PC:0x%x", r.target);
    }
    return "";
}

} // namespace gem5
