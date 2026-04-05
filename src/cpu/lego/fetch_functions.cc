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

#include "cpu/lego/fetch_functions.hh"

#include "base/intmath.hh"
#include "cpu/lego/lego_cpu.hh"
#include "cpu/lego/stage.hh"
#include "cpu/lego/sub_stage.hh"
#include "debug/LegoCPU.hh"
#include "debug/LegoCPUFunc.hh"

namespace gem5
{

// --------- FetchAddressGen ---------

FetchAddressGen::FetchAddressGen(const Params &params)
    : StageFunctionTranslation(params),
      fetchAddrOut("fetchAddr", 0),
      redirectIn("redirect", 0, this),
      fetchWidth(params.fetchWidth),
      nextSeqNum(1),
      waitingForTranslation(false)
{
    fatal_if(!isPowerOf2(fetchWidth),
             "fetchWidth must be a power of 2, got %d", fetchWidth);

    registerPort("fetchAddr", &fetchAddrOut);
    registerPort("redirect", &redirectIn);
}

void
FetchAddressGen::latchInputs()
{
    // Copy cross-stage redirect at cycle boundary
    if (redirectIn.hasData() &&
        redirectIn.getWriterStageId() != getStageId()) {
        localRedirect = redirectIn.read();
        localRedirectValid = true;
    }
}

void
FetchAddressGen::compute()
{
    DPRINTF(LegoCPUFunc, "FetchAddressGen::compute() called, "
            "waitingForTranslation=%d\n", waitingForTranslation);

    if (waitingForTranslation)
        return;

    // Same-stage redirect: update local copy immediately
    if (redirectIn.hasData() &&
        redirectIn.getWriterStageId() == getStageId()) {
        localRedirect = redirectIn.read();
        localRedirectValid = true;
    }

    // Wait for PCUpdate to provide the next PC, unless this
    // is the very first fetch (no redirect received yet).
    if (!localRedirectValid && nextSeqNum > 1)
        return;

    Addr pc;
    if (localRedirectValid) {
        // Use the PC from PCUpdate (sequential or branch)
        pc = localRedirect.target;
        localRedirectValid = false;  // consume it
    } else {
        // First fetch: read initial PC from ThreadContext
        pc = _subStage->getThread(0)->getTC()->pcState().instAddr();
    }

    // Skip if we already output this PC
    if (fetchAddrOut.hasData() &&
        fetchAddrOut.read().pc == pc)
        return;

    // Align to fetch width
    Addr aligned_pc = pc & ~(Addr)(fetchWidth - 1);

    // Create translation request
    translationReq = std::make_shared<Request>();
    translationReq->setVirt(aligned_pc, fetchWidth,
                            Request::INST_FETCH,
                            _subStage->stage()->cpu()->instRequestorId(),
                            pc);

    DPRINTF(LegoCPUFunc, "FetchAddressGen: sending translation for "
            "aligned_pc=0x%x pc=0x%x\n", aligned_pc, pc);

    DPRINTF(LegoCPUFunc, "FetchAddressGen: set waitingForTranslation=true\n");
    waitingForTranslation = true;

    // Send to MMU via the chain
    _subStage->sendTranslationToStage(
        translationReq, getTranslationCallback(),
        BaseMMU::Execute);

    DPRINTF(LegoCPUFunc, "FetchAddressGen: after sendTranslation, "
            "waitingForTranslation=%d\n", waitingForTranslation);
}

void
FetchAddressGen::translationComplete(const Fault &fault,
    const RequestPtr &req, ThreadContext *tc, BaseMMU::Mode mode)
{
    DPRINTF(LegoCPUFunc, "FetchAddressGen::translationComplete() called, "
            "fault=%s vaddr=0x%x paddr=0x%x\n",
            fault == NoFault ? "none" : fault->name(),
            req->getVaddr(), req->getPaddr());
    waitingForTranslation = false;

    if (fault != NoFault) {
        warn("FetchAddressGen: translation fault\n");
        return;
    }

    FetchAddr result;
    result.seqNum = nextSeqNum++;
    result.pc = tc->pcState().instAddr();  // actual instruction PC
    result.paddr = req->getPaddr();
    result.size = fetchWidth;

    fetchAddrOut.write(result);

    DPRINTF(LegoCPUFunc, "FetchAddressGen: seq=%d pc=0x%x paddr=0x%x\n",
            result.seqNum, result.pc, result.paddr);
}

void
FetchAddressGen::flush()
{
    waitingForTranslation = false;
    translationReq = nullptr;
    fetchAddrOut.clear();
}

std::string
FetchAddressGen::traceStatus() const
{
    if (waitingForTranslation)
        return "F:wait(MMU)";
    if (fetchAddrOut.hasData())
        return csprintf("F:0x%x", fetchAddrOut.read().pc);
    return "";
}

// --------- FetchMemRequest ---------

FetchMemRequest::FetchMemRequest(const Params &params)
    : StageFunctionWithPort(params),
      fetchAddrIn("fetchAddr", 0, this),
      fetchLineOut("fetchLine", 0),
      waitingForCache(false),
      lastRequestedSeqNum(0)
{
    registerPort("fetchAddr", &fetchAddrIn);
    registerPort("fetchLine", &fetchLineOut);
}

void
FetchMemRequest::compute()
{
    DPRINTF(LegoCPUFunc, "FetchMemRequest::compute() called, "
            "waitingForCache=%d hasData=%d\n",
            waitingForCache, fetchAddrIn.hasData());

    if (waitingForCache)
        return;

    if (!fetchAddrIn.hasData())
        return;

    const FetchAddr &addr = fetchAddrIn.read();
    DPRINTF(LegoCPUFunc, "FetchMemRequest: read addr seq=%d pc=0x%x "
            "paddr=0x%x size=%d\n",
            addr.seqNum, addr.pc, addr.paddr, addr.size);

    // Don't re-request the same instruction
    if (addr.seqNum == lastRequestedSeqNum)
        return;

    lastRequestedSeqNum = addr.seqNum;

    // If we already have this fetch line cached, output immediately
    if (fetchLineOut.hasData()) {
        const FetchLine &cached = fetchLineOut.read();
        Addr alignedAddr = addr.pc & ~(Addr)(addr.size - 1);
        if (cached.lineBaseAddr == alignedAddr) {
            FetchLine result = cached;
            result.seqNum = addr.seqNum;
            result.pc = addr.pc;
            fetchLineOut.write(result);
            DPRINTF(LegoCPUFunc, "FetchMemRequest: cache hit seq=%d "
                    "pc=0x%x (reuse line 0x%x)\n",
                    addr.seqNum, addr.pc, alignedAddr);
            return;
        }
    }

    // Create the icache request packet
    auto req = std::make_shared<Request>(
        addr.paddr, addr.size, Request::INST_FETCH,
        /* requestorId — will be set by CPU */ 0);

    PacketPtr pkt = new Packet(req, MemCmd::ReadReq);
    pkt->allocate();

    // Push sender state for response routing
    pkt->pushSenderState(
        new FuncSenderState(this, addr.seqNum, ICACHE));

    // Send up the chain
    _subStage->sendPacketToStage(pkt);

    waitingForCache = true;

    DPRINTF(LegoCPUFunc, "FetchMemRequest: seq=%d paddr=0x%x\n",
            addr.seqNum, addr.paddr);
}

void
FetchMemRequest::recvTimingResp(PacketPtr pkt)
{
    auto *ss = safe_cast<FuncSenderState *>(
        pkt->popSenderState());

    FetchLine result;
    result.seqNum = ss->instSeqNum;
    result.pc = fetchAddrIn.hasData() ? fetchAddrIn.read().pc : 0;
    result.validBytes = pkt->getSize();
    // Use aligned virtual address as base, not physical
    result.lineBaseAddr = result.pc & ~(Addr)(result.validBytes - 1);

    const uint8_t *pktData = pkt->getConstPtr<uint8_t>();
    result.data.assign(pktData, pktData + pkt->getSize());

    delete ss;
    delete pkt;

    waitingForCache = false;
    fetchLineOut.write(result);

    DPRINTF(LegoCPUFunc, "FetchMemRequest: resp seq=%d pc=0x%x bytes=%d\n",
            result.seqNum, result.pc, result.validBytes);
}

void
FetchMemRequest::flush()
{
    waitingForCache = false;
    lastRequestedSeqNum = 0;
    fetchLineOut.clear();
}

std::string
FetchMemRequest::traceStatus() const
{
    if (waitingForCache)
        return "F:wait(icache)";
    if (fetchLineOut.hasData())
        return csprintf("F:got(0x%x)", fetchLineOut.read().pc);
    return "";
}

} // namespace gem5
