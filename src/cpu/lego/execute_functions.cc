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
    if (fetchLineIn.hasData() &&
        fetchLineIn.getWriterStageId() != getStageId()) {
        const FetchLine &src = fetchLineIn.read();
        if (src.seqNum != localFetchLine.seqNum) {
            DPRINTF(LegoCPUFunc, "InstructionDecode::latchInputs() "
                    "latching seq=%d pc=0x%x base=0x%x size=%d\n",
                    src.seqNum, src.pc, src.lineBaseAddr,
                    src.data.size());
            localFetchLine = src;
            localFetchLineValid = true;
        }
    }
}

void
InstructionDecode::compute()
{
    if (decodedInstOut.isBlocked()) {
        fetchLineIn.blockSource();
        return;
    }
    fetchLineIn.unblockSource();

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

    // Use the fetch line's PC for decoding, not TC's pcState
    // (TC may have been advanced by PCUpdate already).
    // Create a pcState matching the instruction we're decoding.
    auto decodePC = tc->pcState().clone();
    decodePC->set(pcAddr);
    decoder->moreBytes(*decodePC, line.lineBaseAddr + offset);

    StaticInstPtr si = nullptr;
    if (decoder->instReady()) {
        si = decoder->decode(*decodePC);
    }

    DecodedInst result;
    result.seqNum = line.seqNum;
    result.staticInst = si;
    result.pc = line.pc;

    decodedInstOut.write(result);
    localFetchLineValid = false;

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
ALUExecute::latchInputs()
{
    if (decodedInstIn.hasData() &&
        decodedInstIn.getWriterStageId() != getStageId()) {
        const DecodedInst &src = decodedInstIn.read();
        if (src.seqNum != localDecodedInst.seqNum) {
            localDecodedInst = src;
            localDecodedInstValid = true;
        }
    }
}

void
ALUExecute::compute()
{
    if (execResultOut.isBlocked()) {
        decodedInstIn.blockSource();
        return;
    }
    decodedInstIn.unblockSource();

    // Same-stage: update local copy immediately
    if (decodedInstIn.hasData() &&
        decodedInstIn.getWriterStageId() == getStageId()) {
        localDecodedInst = decodedInstIn.read();
        localDecodedInstValid = true;
    }

    if (!localDecodedInstValid)
        return;

    const DecodedInst &inst = localDecodedInst;

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

    execResultOut.write(result);
    localDecodedInstValid = false;

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
PCUpdate::latchInputs()
{
    if (execResultIn.hasData() &&
        execResultIn.getWriterStageId() != getStageId()) {
        const ExecResult &src = execResultIn.read();
        if (src.seqNum != localExecResult.seqNum) {
            localExecResult = src;
            localExecResultValid = true;
        }
    }
}

void
PCUpdate::compute()
{
    if (redirectOut.isBlocked()) {
        execResultIn.blockSource();
        return;
    }
    execResultIn.unblockSource();

    // Same-stage: update local copy immediately
    if (execResultIn.hasData() &&
        execResultIn.getWriterStageId() == getStageId()) {
        localExecResult = execResultIn.read();
        localExecResultValid = true;
    }

    bool branchTaken = false;
    Addr target = 0;

    if (localExecResultValid &&
        localExecResult.seqNum != lastUpdatedSeqNum) {
        // New instruction executed — advance PC using actual
        // instruction and condition flags from execute
        const ExecResult &result = localExecResult;
        lastUpdatedSeqNum = result.seqNum;

        ThreadContext *tc = _subStage->getThread(0)->getTC();

        speculativePC.reset(tc->pcState().clone());
        result.staticInst->advancePC(*speculativePC);
        tc->pcState(*speculativePC);

        target = speculativePC->instAddr();
        lastStaticInst = result.staticInst;
        localExecResultValid = false;

        // Branch taken = control instruction that went
        // somewhere other than sequential PC+instSize
        if (result.staticInst->isControl()) {
            Addr sequential = result.pc + result.staticInst->size();
            branchTaken = (target != sequential);
        }

        if (branchTaken) {
            DPRINTF(LegoCPUFunc, "PCUpdate: branch taken to 0x%x "
                    "(from 0x%x)\n", target, result.pc);
        } else {
            DPRINTF(LegoCPUFunc, "PCUpdate: sequential 0x%x -> "
                    "0x%x\n", result.pc, target);
        }
    } else if (speculativePC && lastStaticInst &&
               redirectOut.getLastWritten() != curTick()) {
        // No new exec result AND we didn't already produce
        // a redirect this tick — speculatively advance PC
        lastStaticInst->advancePC(*speculativePC);
        target = speculativePC->instAddr();

        DPRINTF(LegoCPUFunc, "PCUpdate: speculative to 0x%x\n",
                target);
    } else {
        return;
    }

    lastRedirectSeqNum++;

    Redirect redir;
    redir.seqNum = lastRedirectSeqNum;
    redir.target = target;
    redir.valid = branchTaken;

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
