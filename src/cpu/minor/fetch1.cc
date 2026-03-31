/*
 * Copyright (c) 2013-2014 ARM Limited
 * All rights reserved
 *
 * The license below extends only to copyright in the software and shall
 * not be construed as granting a license to any other intellectual
 * property including but not limited to intellectual property relating
 * to a hardware implementation of the functionality of the software
 * licensed hereunder.  You may use the software subject to the license
 * terms below provided that you ensure that this notice is replicated
 * unmodified and in its entirety in all distributions of the software,
 * modified or unmodified, in source code or in binary form.
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

#include "cpu/minor/fetch1.hh"

#include <cstring>
#include <iomanip>
#include <sstream>

#include "arch/generic/decoder.hh"
#include "base/cast.hh"
#include "base/compiler.hh"
#include "base/logging.hh"
#include "base/trace.hh"
#include "cpu/minor/fetch2.hh"
#include "cpu/minor/pipeline.hh"
#include "debug/Drain.hh"
#include "debug/Fetch.hh"
#include "debug/MinorTrace.hh"

namespace gem5
{

namespace minor
{

Fetch1::Fetch1(const std::string &name_,
    MinorCPU &cpu_,
    const BaseMinorCPUParams &params,
    Latch<BranchData>::Output inp_,
    Latch<ForwardLineData>::Input out_,
    Latch<BranchData>::Output prediction_,
    std::vector<InputBuffer<ForwardLineData>> &next_stage_input_buffer) :
    Named(name_),
    cpu(cpu_),
    inp(inp_),
    out(out_),
    prediction(prediction_),
    nextStageReserve(next_stage_input_buffer),
    icachePort(name_ + ".icache_port", *this, cpu_),
    lineSnap(params.fetch1LineSnapWidth),
    maxLineWidth(params.fetch1LineWidth),
    fetchLimit(params.fetch1FetchLimit),
    fetchInfo(params.numThreads),
    threadPriority(0),
    requests(name_ + ".requests", "lines", params.fetch1FetchLimit),
    transfers(name_ + ".transfers", "lines", params.fetch1FetchLimit),
    icacheState(IcacheRunning),
    lineSeqNum(InstId::firstLineSeqNum),
    numFetchesInMemorySystem(0),
    numFetchesInITLB(0)
{
    for (auto &info: fetchInfo)
        info.pc.reset(params.isa[0]->newPCState());

    if (lineSnap == 0) {
        lineSnap = cpu.cacheLineSize();
        DPRINTF(Fetch, "lineSnap set to cache line size of: %d\n",
            lineSnap);
    }

    if (maxLineWidth == 0) {
        maxLineWidth = cpu.cacheLineSize();
        DPRINTF(Fetch, "maxLineWidth set to cache line size of: %d\n",
            maxLineWidth);
    }

    size_t inst_size = cpu.threads[0]->decoder->moreBytesSize();

    /* These assertions should be copied to the Python config. as well */
    if ((lineSnap % inst_size) != 0) {
        fatal("%s: fetch1LineSnapWidth must be a multiple "
            "of the inst width (%d)\n", name_,
            inst_size);
    }

    if ((maxLineWidth >= lineSnap && (maxLineWidth % inst_size)) != 0) {
        fatal("%s: fetch1LineWidth must be a multiple of"
            " the inst width (%d), and >= fetch1LineSnapWidth (%d)\n",
            name_, inst_size, lineSnap);
    }

    if (fetchLimit < 1) {
        fatal("%s: fetch1FetchLimit must be >= 1 (%d)\n", name_,
            fetchLimit);
    }
}

inline ThreadID
Fetch1::getScheduledThread()
{
    /* Select thread via policy. */
    std::vector<ThreadID> priority_list;

    switch (cpu.threadPolicy) {
      case enums::SingleThreaded:
        priority_list.push_back(0);
        break;
      case enums::RoundRobin:
        priority_list = cpu.roundRobinPriority(threadPriority);
        break;
      case enums::Random:
        priority_list = cpu.randomPriority();
        break;
      default:
        panic("Unknown fetch policy");
    }

    for (auto tid : priority_list) {
        if (cpu.getContext(tid)->status() == ThreadContext::Active &&
            !fetchInfo[tid].blocked &&
            fetchInfo[tid].state == FetchRunning) {
            threadPriority = tid;
            return tid;
        }
    }

   return InvalidThreadID;
}

void
Fetch1::fetchLine(ThreadID tid)
{
    /* Reference the currently used thread state. */
    Fetch1ThreadInfo &thread = fetchInfo[tid];

    /* If line_offset != 0, a request is pushed for the remainder of the
     * line. */
    /* Use a lower, sizeof(MachInst) aligned address for the fetch */
    Addr aligned_pc = thread.fetchAddr & ~((Addr) lineSnap - 1);
    unsigned int line_offset = aligned_pc % lineSnap;
    unsigned int request_size = maxLineWidth - line_offset;

    /* Fill in the line's id */
    InstId request_id(tid,
        thread.streamSeqNum, thread.predictionSeqNum,
        lineSeqNum);

    FetchRequestPtr request = new FetchRequest(*this, request_id,
            thread.fetchAddr);

    DPRINTF(Fetch, "Inserting fetch into the fetch queue "
        "%s addr: 0x%x pc: %s line_offset: %d request_size: %d\n",
        request_id, aligned_pc, thread.fetchAddr, line_offset, request_size);

    request->request->setContext(cpu.threads[tid]->getTC()->contextId());
    request->request->setVirt(
        aligned_pc, request_size, Request::INST_FETCH, cpu.instRequestorId(),
        /* I've no idea why we need the PC, but give it */
        thread.fetchAddr);

    DPRINTF(Fetch, "Submitting ITLB request\n");
    numFetchesInITLB++;

    request->state = FetchRequest::InTranslation;

    /* Reserve space in the queues upstream of requests for results */
    transfers.reserve();
    requests.push(request);

    /* Submit the translation request.  The response will come
     *  through finish/markDelayed on this request as it bears
     *  the Translation interface */
    cpu.threads[request->id.threadId]->mmu->translateTiming(
        request->request,
        cpu.getContext(request->id.threadId),
        request, BaseMMU::Execute);

    lineSeqNum++;

    /* Step the PC for the next line onto the line aligned next address.
     * Note that as instructions can span lines, this PC is only a
     * reliable 'new' PC if the next line has a new stream sequence number. */
    thread.fetchAddr = aligned_pc + request_size;
}

std::ostream &
operator <<(std::ostream &os, Fetch1::IcacheState state)
{
    switch (state) {
      case Fetch1::IcacheRunning:
        os << "IcacheRunning";
        break;
      case Fetch1::IcacheNeedsRetry:
        os << "IcacheNeedsRetry";
        break;
      default:
        os << "IcacheState-" << static_cast<int>(state);
        break;
    }
    return os;
}

void
Fetch1::FetchRequest::makePacket()
{
    /* Make the necessary packet for a memory transaction */
    packet = new Packet(request, MemCmd::ReadReq);
    packet->allocate();

    /* This FetchRequest becomes SenderState to allow the response to be
     *  identified */
    packet->pushSenderState(this);
}

void
Fetch1::FetchRequest::finish(const Fault &fault_, const RequestPtr &request_,
                             ThreadContext *tc, BaseMMU::Mode mode)
{
    fault = fault_;

    state = Translated;
    fetch.handleTLBResponse(this);

    /* Let's try and wake up the processor for the next cycle */
    fetch.cpu.wakeupOnEvent(Pipeline::Fetch1StageId);
}

void
Fetch1::handleTLBResponse(FetchRequestPtr response)
{
    numFetchesInITLB--;

    if (response->fault != NoFault) {
        DPRINTF(Fetch, "Fault in address ITLB translation: %s, "
            "paddr: 0x%x, vaddr: 0x%x\n",
            response->fault->name(),
            (response->request->hasPaddr() ?
                response->request->getPaddr() : 0),
            response->request->getVaddr());

        if (debug::MinorTrace)
            minorTraceResponseLine(name(), response);
    } else {
        DPRINTF(Fetch, "Got ITLB response\n");
    }

    response->state = FetchRequest::Translated;

    tryToSendToTransfers(response);
}

Fetch1::FetchRequest::~FetchRequest()
{
    if (packet)
        delete packet;
}

void
Fetch1::tryToSendToTransfers(FetchRequestPtr request)
{
    if (!requests.empty() && requests.front() != request) {
        DPRINTF(Fetch, "Fetch not at front of requests queue, can't"
            " issue to memory\n");
        return;
    }

    if (request->state == FetchRequest::InTranslation) {
        DPRINTF(Fetch, "Fetch still in translation, not issuing to"
            " memory\n");
        return;
    }

    if (request->isDiscardable() || request->fault != NoFault) {
        /* Discarded and faulting requests carry on through transfers
         *  as Complete/packet == NULL */

        request->state = FetchRequest::Complete;
        moveFromRequestsToTransfers(request);

        /* Wake up the pipeline next cycle as there will be no event
         *  for this queue->queue transfer */
        cpu.wakeupOnEvent(Pipeline::Fetch1StageId);
    } else if (request->state == FetchRequest::Translated) {
        if (!request->packet)
            request->makePacket();

        /* Ensure that the packet won't delete the request */
        assert(request->packet->needsResponse());

        if (tryToSend(request))
            moveFromRequestsToTransfers(request);
    } else {
        DPRINTF(Fetch, "Not advancing line fetch\n");
    }
}

void
Fetch1::moveFromRequestsToTransfers(FetchRequestPtr request)
{
    assert(!requests.empty() && requests.front() == request);

    requests.pop();
    transfers.push(request);
}

bool
Fetch1::tryToSend(FetchRequestPtr request)
{
    bool ret = false;

    if (icachePort.sendTimingReq(request->packet)) {
        /* Invalidate the fetch_requests packet so we don't
         *  accidentally fail to deallocate it (or use it!)
         *  later by overwriting it */
        request->packet = NULL;
        request->state = FetchRequest::RequestIssuing;
        numFetchesInMemorySystem++;

        ret = true;

        DPRINTF(Fetch, "Issued fetch request to memory: %s\n",
            request->id);
    } else {
        /* Needs to be resent, wait for that */
        icacheState = IcacheNeedsRetry;

        DPRINTF(Fetch, "Line fetch needs to retry: %s\n",
            request->id);
    }

    return ret;
}

void
Fetch1::stepQueues()
{
    IcacheState old_icache_state = icacheState;

    switch (icacheState) {
      case IcacheRunning:
        /* Move ITLB results on to the memory system */
        if (!requests.empty()) {
            tryToSendToTransfers(requests.front());
        }
        break;
      case IcacheNeedsRetry:
        break;
    }

    if (icacheState != old_icache_state) {
        DPRINTF(Fetch, "Step in state %s moving to state %s\n",
            old_icache_state, icacheState);
    }
}

void
Fetch1::popAndDiscard(FetchQueue &queue)
{
    if (!queue.empty()) {
        delete queue.front();
        queue.pop();
    }
}

unsigned int
Fetch1::numInFlightFetches()
{
    return requests.occupiedSpace() +
        transfers.occupiedSpace();
}

/** Print the appropriate MinorLine line for a fetch response */
void
Fetch1::minorTraceResponseLine(const std::string &name,
    Fetch1::FetchRequestPtr response) const
{
    const RequestPtr &request = response->request;

    if (response->packet && response->packet->isError()) {
        minorLine(*this, "id=F;%s vaddr=0x%x fault=\"error packet\"\n",
            response->id, request->getVaddr());
    } else if (response->fault != NoFault) {
        minorLine(*this, "id=F;%s vaddr=0x%x fault=\"%s\"\n",
            response->id, request->getVaddr(), response->fault->name());
    } else {
        minorLine(*this, "id=%s size=%d vaddr=0x%x paddr=0x%x\n",
            response->id, request->getSize(),
            request->getVaddr(), request->getPaddr());
    }
}

bool
Fetch1::recvTimingResp(PacketPtr response)
{
    DPRINTF(Fetch, "recvTimingResp %d\n", numFetchesInMemorySystem);

    /* Only push the response if we didn't change stream?  No,  all responses
     *  should hit the responses queue.  It's the job of 'step' to throw them
     *  away. */
    FetchRequestPtr fetch_request = safe_cast<FetchRequestPtr>
        (response->popSenderState());

    /* Fixup packet in fetch_request as this may have changed */
    assert(!fetch_request->packet);
    fetch_request->packet = response;

    numFetchesInMemorySystem--;
    fetch_request->state = FetchRequest::Complete;

    if (debug::MinorTrace)
        minorTraceResponseLine(name(), fetch_request);

    if (response->isError()) {
        DPRINTF(Fetch, "Received error response packet: %s\n",
            fetch_request->id);
    }

    /* We go to idle even if there are more things to do on the queues as
     *  it's the job of step to actually step us on to the next transaction */

    /* Let's try and wake up the processor for the next cycle to move on
     *  queues */
    cpu.wakeupOnEvent(Pipeline::Fetch1StageId);

    /* Never busy */
    return true;
}

void
Fetch1::recvReqRetry()
{
    DPRINTF(Fetch, "recvRetry\n");
    assert(icacheState == IcacheNeedsRetry);
    assert(!requests.empty());

    FetchRequestPtr retryRequest = requests.front();

    icacheState = IcacheRunning;

    if (tryToSend(retryRequest))
        moveFromRequestsToTransfers(retryRequest);
}

std::ostream &
operator <<(std::ostream &os, Fetch1::FetchState state)
{
    switch (state) {
      case Fetch1::FetchHalted:
        os << "FetchHalted";
        break;
      case Fetch1::FetchWaitingForPC:
        os << "FetchWaitingForPC";
        break;
      case Fetch1::FetchRunning:
        os << "FetchRunning";
        break;
      default:
        os << "FetchState-" << static_cast<int>(state);
        break;
    }
    return os;
}

void
Fetch1::changeStream(const BranchData &branch)
{
    Fetch1ThreadInfo &thread = fetchInfo[branch.threadId];

    updateExpectedSeqNums(branch);

    /* Start fetching again if we were stopped */
    switch (branch.reason) {
      case BranchData::SuspendThread:
        {
            if (thread.wakeupGuard) {
                DPRINTF(Fetch, "Not suspending fetch due to guard: %s\n",
                                branch);
            } else {
                DPRINTF(Fetch, "Suspending fetch: %s\n", branch);
                thread.state = FetchWaitingForPC;
            }
        }
        break;
      case BranchData::HaltFetch:
        DPRINTF(Fetch, "Halting fetch\n");
        thread.state = FetchHalted;
        break;
      default:
        DPRINTF(Fetch, "Changing stream on branch: %s\n", branch);
        thread.state = FetchRunning;
        break;
    }
    set(thread.pc, branch.target);
    thread.fetchAddr = thread.pc->instAddr();
}

void
Fetch1::updateExpectedSeqNums(const BranchData &branch)
{
    Fetch1ThreadInfo &thread = fetchInfo[branch.threadId];

    DPRINTF(Fetch, "Updating streamSeqNum from: %d to %d,"
        " predictionSeqNum from: %d to %d\n",
        thread.streamSeqNum, branch.newStreamSeqNum,
        thread.predictionSeqNum, branch.newPredictionSeqNum);

    /* Change the stream */
    thread.streamSeqNum = branch.newStreamSeqNum;
    /* Update the prediction.  Note that it's possible for this to
     *  actually set the prediction to an *older* value if new
     *  predictions have been discarded by execute */
    thread.predictionSeqNum = branch.newPredictionSeqNum;
}

void
Fetch1::processResponse(Fetch1::FetchRequestPtr response,
    ForwardLineData &line)
{
    Fetch1ThreadInfo &thread = fetchInfo[response->id.threadId];
    PacketPtr packet = response->packet;

    /* Pass the prefetch abort (if any) on to Fetch2 in a ForwardLineData
     * structure */
    line.setFault(response->fault);
    /* Make sequence numbers valid in return */
    line.id = response->id;
    /* Set the PC in case there was a sequence change */
    set(line.pc, thread.pc);
    /* Set fetch address to virtual address */
    line.fetchAddr = response->pc;
    /* Set the lineBase, which is a sizeof(MachInst) aligned address <=
     *  pc.instAddr() */
    line.lineBaseAddr = response->request->getVaddr();

    if (response->fault != NoFault) {
        /* Stop fetching if there was a fault */
        /* Should probably try to flush the queues as well, but we
         * can't be sure that this fault will actually reach Execute, and we
         * can't (currently) selectively remove this stream from the queues */
        DPRINTF(Fetch, "Stopping line fetch because of fault: %s\n",
            response->fault->name());
        thread.state = Fetch1::FetchWaitingForPC;
    } else {
        line.adoptPacketData(packet);
        /* Null the response's packet to prevent the response from trying to
         *  deallocate the packet */
        response->packet = NULL;
    }
}

Fetch1::~Fetch1()
{
}

void
Fetch1::evaluate()
{
    ForwardLineData &line_out = *out.inputWire;
    assert(line_out.isBubble());
    evaluateCore(*inp.outputWire, *prediction.outputWire, line_out);
}

void
Fetch1::handleBranchRedirects(const BranchData &execute_branch,
                              const BranchData &fetch2_branch)
{
    /** Are both branches from later stages valid and for the same thread? */
    if (execute_branch.threadId != InvalidThreadID &&
        execute_branch.threadId == fetch2_branch.threadId) {

        Fetch1ThreadInfo &thread = fetchInfo[execute_branch.threadId];

        /* Are we changing stream?  Look to the Execute branches first, then
         * to predicted changes of stream from Fetch2 */
        if (execute_branch.isStreamChange()) {
            if (thread.state == FetchHalted) {
                DPRINTF(Fetch, "Halted, ignoring branch: %s\n", execute_branch);
            } else {
                changeStream(execute_branch);
            }

            if (!fetch2_branch.isBubble()) {
                DPRINTF(Fetch, "Ignoring simultaneous prediction: %s\n",
                    fetch2_branch);
            }

            /* The streamSeqNum tagging in request/response ->req should handle
             *  discarding those requests when we get to them. */
        } else if (thread.state != FetchHalted && fetch2_branch.isStreamChange()) {
            /* Handle branch predictions by changing the instruction source
             * if we're still processing the same stream (as set by streamSeqNum)
             * as the one of the prediction.
             */
            if (fetch2_branch.newStreamSeqNum != thread.streamSeqNum) {
                DPRINTF(Fetch, "Not changing stream on prediction: %s,"
                    " streamSeqNum mismatch\n",
                    fetch2_branch);
            } else {
                changeStream(fetch2_branch);
            }
        }
    } else {
        /* Fetch2 and Execute branches are for different threads */
        if (execute_branch.threadId != InvalidThreadID &&
            execute_branch.isStreamChange()) {

            if (fetchInfo[execute_branch.threadId].state == FetchHalted) {
                DPRINTF(Fetch, "Halted, ignoring branch: %s\n", execute_branch);
            } else {
                changeStream(execute_branch);
            }
        }

        if (fetch2_branch.threadId != InvalidThreadID &&
            fetch2_branch.isStreamChange()) {

            if (fetchInfo[fetch2_branch.threadId].state == FetchHalted) {
                DPRINTF(Fetch, "Halted, ignoring branch: %s\n", fetch2_branch);
            } else if (fetch2_branch.newStreamSeqNum != fetchInfo[fetch2_branch.threadId].streamSeqNum) {
                DPRINTF(Fetch, "Not changing stream on prediction: %s,"
                    " streamSeqNum mismatch\n", fetch2_branch);
            } else {
                changeStream(fetch2_branch);
            }
        }
    }
}

ThreadID
Fetch1::tryToFetch()
{
    if (numInFlightFetches() < fetchLimit) {
        ThreadID fetch_tid = getScheduledThread();

        if (fetch_tid != InvalidThreadID) {
            DPRINTF(Fetch, "Fetching from thread %d\n", fetch_tid);

            /* Generate fetch to selected thread */
            fetchLine(fetch_tid);
            return fetch_tid;
        } else {
            DPRINTF(Fetch, "No active threads available to fetch from\n");
        }
    }

    return InvalidThreadID;
}

void
Fetch1::processCompletedFetch(ForwardLineData &line_out,
                              bool &was_discarded,
                              ThreadID &discarded_tid)
{
    was_discarded = false;

    /* As we've thrown away early lines, if there is a line, it must
     *  be from the right stream */
    if (!transfers.empty() &&
        transfers.front()->isComplete())
    {
        Fetch1::FetchRequestPtr response = transfers.front();

        if (response->isDiscardable()) {
            was_discarded = true;
            discarded_tid = response->id.threadId;

            DPRINTF(Fetch, "Discarding translated fetch as it's for"
                " an old stream\n");

            /* Wake up next cycle just in case there was some other
             *  action to do */
            cpu.wakeupOnEvent(Pipeline::Fetch1StageId);
        } else {
            DPRINTF(Fetch, "Processing fetched line: %s\n",
                response->id);

            processResponse(response, line_out);
        }

        popAndDiscard(transfers);
    }
}

void
Fetch1::postEvaluate(const ForwardLineData &line_out)
{
    /* If we generated output, mark the stage as being active
     *  to encourage that output on to the next stage */
    if (!line_out.isBubble())
        cpu.activityRecorder->activity();

    /* This looks hackish.  And it is, but there doesn't seem to be a better
     * way to do this.  The signal from commit to suspend fetch takes 1
     * clock cycle to propagate to fetch.  However, a legitimate wakeup
     * may occur between cycles from the memory system.  Thus wakeup guard
     * prevents us from suspending in that case. */

    for (auto& thread : fetchInfo) {
        thread.wakeupGuard = false;
    }
}

void
Fetch1::evaluateCore(const BranchData &execute_branch,
                     const BranchData &fetch2_branch,
                     ForwardLineData &line_out)
{
    for (ThreadID tid = 0; tid < cpu.numThreads; tid++)
        fetchInfo[tid].blocked = !nextStageReserve[tid].canReserve();

    handleBranchRedirects(execute_branch, fetch2_branch);

    ThreadID fetch_tid = tryToFetch();
    if (fetch_tid != InvalidThreadID)
        nextStageReserve[fetch_tid].reserve();

    stepQueues();

    bool was_discarded = false;
    ThreadID discarded_tid = InvalidThreadID;
    processCompletedFetch(line_out, was_discarded, discarded_tid);
    if (was_discarded)
        nextStageReserve[discarded_tid].freeReservation();

    postEvaluate(line_out);
}

void
Fetch1::wakeupFetch(ThreadID tid)
{
    ThreadContext *thread_ctx = cpu.getContext(tid);
    Fetch1ThreadInfo &thread = fetchInfo[tid];
    set(thread.pc, thread_ctx->pcState());
    thread.fetchAddr = thread.pc->instAddr();
    thread.state = FetchRunning;
    thread.wakeupGuard = true;
    DPRINTF(Fetch, "[tid:%d]: Changing stream wakeup %s\n", tid, *thread.pc);

    cpu.wakeupOnEvent(Pipeline::Fetch1StageId);
}

bool
Fetch1::isDrained()
{
    bool drained = numInFlightFetches() == 0 && (*out.inputWire).isBubble();
    for (ThreadID tid = 0; tid < cpu.numThreads; tid++) {
        Fetch1ThreadInfo &thread = fetchInfo[tid];
        DPRINTF(Drain, "isDrained[tid:%d]: %s %s%s\n",
                tid,
                thread.state == FetchHalted,
                (numInFlightFetches() == 0 ? "" : "inFlightFetches "),
                ((*out.inputWire).isBubble() ? "" : "outputtingLine"));

        drained = drained && (thread.state != FetchRunning);
    }

    return drained;
}

void
Fetch1::FetchRequest::reportData(std::ostream &os) const
{
    os << id;
}

bool Fetch1::FetchRequest::isDiscardable() const
{
    Fetch1ThreadInfo &thread = fetch.fetchInfo[id.threadId];

    /* Can't discard lines in TLB/memory */
    return state != InTranslation && state != RequestIssuing &&
        (id.streamSeqNum != thread.streamSeqNum ||
        id.predictionSeqNum != thread.predictionSeqNum);
}

void
Fetch1::minorTrace() const
{
    // TODO: Un-bork minorTrace for THREADS
    // bork bork bork
    const Fetch1ThreadInfo &thread = fetchInfo[0];

    std::ostringstream data;

    if (thread.blocked)
        data << 'B';
    else
        (*out.inputWire).reportData(data);

    minor::minorTrace("state=%s icacheState=%s in_tlb_mem=%s/%s"
        " streamSeqNum=%d lines=%s\n", thread.state, icacheState,
        numFetchesInITLB, numFetchesInMemorySystem,
        thread.streamSeqNum, data.str());
    requests.minorTrace();
    transfers.minorTrace();
}

bool
SingleStageFetch1::recvTimingResp(PacketPtr pkt)
{
    DPRINTF(Fetch, "recvTimingResp %d\n", numFetchesInMemorySystem);

    FetchRequestPtr fetch_request = safe_cast<FetchRequestPtr>
        (pkt->popSenderState());

    assert(!fetch_request->packet);
    fetch_request->packet = pkt;

    numFetchesInMemorySystem--;
    fetch_request->state = FetchRequest::Complete;

    if (debug::MinorTrace)
        minorTraceResponseLine(name(), fetch_request);

    if (pkt->isError()) {
        DPRINTF(Fetch, "Received error response packet: %s\n",
            fetch_request->id);
    }

    /* Use immediate wakeup so the pipeline evaluates at the current
     * clock edge instead of waiting until the next cycle. */
    cpu.wakeupOnEventImmediate(Pipeline::Fetch1StageId);

    return true;
}

SingleStageFetch1::SingleStageFetch1(const std::string &name_,
    MinorCPU &cpu_,
    const BaseMinorCPUParams &params,
    Latch<BranchData>::Output inp_,
    Latch<ForwardLineData>::Input out_,
    Latch<BranchData>::Output prediction_,
    std::vector<InputBuffer<ForwardLineData>> &next_stage_input_buffer,
    SingleStageFetch2 *fetch2_,
    Latch<BranchData>::Input eToF1Input_) :
    Fetch1(name_, cpu_, params, inp_, out_, prediction_,
        next_stage_input_buffer),
    fetch2(fetch2_),
    eToF1Input(eToF1Input_)
{
}

void
SingleStageFetch1::evaluate()
{
    /* --- Fetch1 phase: I-cache management --- */

    /* Check back-pressure from Decode (the real next stage) */
    for (ThreadID tid = 0; tid < cpu.numThreads; tid++)
        fetchInfo[tid].blocked = fetch2->isNextStageBlocked(tid);

    /* Branch redirects: Read Execute's branch result from the
     * eToF1 INPUT wire (same-cycle bypass) instead of the OUTPUT
     * wire (1-cycle delayed).  This models the Cortex-M4's 3-stage
     * pipeline where the branch target feeds back to Fetch within
     * the same clock cycle.  Execute has already evaluated this
     * cycle (Pipeline::evaluate runs Execute before Fetch). */
    {
        const BranchData &execBranch = *eToF1Input.inputWire;
        if (execBranch.isStreamChange()) {
            DPRINTF(Fetch, "SF1: Execute redirect reason=%d target=%#x "
                "inst=[%s] pc=%#x\n",
                execBranch.reason,
                execBranch.target ? execBranch.target->instAddr() : 0,
                *execBranch.inst, execBranch.inst->pc->instAddr());
        }
    }
    handleBranchRedirects(*eToF1Input.inputWire, lastPrediction);

    /* Step I-cache queues */
    stepQueues();

    /* Process completed I-cache response — pop from transfers queue
     * BEFORE tryToFetch so numInFlightFetches sees the freed slot */
    ForwardLineData line;
    bool was_discarded = false;
    ThreadID discarded_tid = InvalidThreadID;
    processCompletedFetch(line, was_discarded, discarded_tid);

    /* --- Handoff: push line to Fetch2's inputBuffer --- */
    if (!line.isBubble()) {
        fetch2->inputBuffer[line.id.threadId].setTail(line);
        fetch2->inputBuffer[line.id.threadId].pushTail();
    }

    /* --- Fetch2 phase: decode + branch prediction --- */
    BranchData prediction;
    fetch2->runDecodeCore(prediction, *eToF1Input.inputWire);

    /* Apply predicted-taken redirect IMMEDIATELY in this cycle,
     * rather than waiting for next cycle via lastPrediction.
     * This models the Cortex-M4's early address resolution:
     * the branch target is computed during decode and the
     * pipeline refill starts in the same cycle — there is no
     * speculative prediction delay on real hardware [DDI0439D §3.3]. */
    if (prediction.isStreamChange()) {
        DPRINTF(Fetch, "SF1: Decode prediction reason=%d target=%#x "
            "inst=[%s] pc=%#x\n",
            prediction.reason,
            prediction.target ? prediction.target->instAddr() : 0,
            *prediction.inst, prediction.inst->pc->instAddr());
        Fetch1ThreadInfo &thread = fetchInfo[prediction.threadId];
        if (thread.state != FetchHalted &&
            prediction.newStreamSeqNum == thread.streamSeqNum) {
            changeStream(prediction);
        }
    } else {
        if (fetch2->forwardedSavedLine!=0) {
            /* savedLine was restored in reactToExecuteBranch.
             * Advance fetchAddr past the savedLine so we fetch the
             * NEXT word (continuation) instead of redundantly
             * re-fetching the same line. Use thread 0 directly —
             * getScheduledThread() can return -1 when blocked. */
            Fetch1ThreadInfo &thread = fetchInfo[0];
            DPRINTF(Fetch, "SF1: savedLine forwarded, advancing "
                "fetchAddr to %#x\n", fetch2->forwardedSavedLine);
            thread.fetchAddr = fetch2->forwardedSavedLine;
        }
    }

    /* Clear lastPrediction — it was consumed immediately above */
    lastPrediction = BranchData::bubble();

    /* Issue new I-cache fetch AFTER decode + branch prediction.
     * If a branch was predicted taken, changeStream() already updated
     * the PC to the branch target, so this fetch is for the correct
     * address.
     *
     * If no prediction was produced but the lookahead scan detects a
     * branch instruction next in the line, suppress the sequential
     * prefetch. */
    if (!prediction.isBubble() || !fetch2->scanNextForBranch()) {
    // if (!prediction.isBubble()) {
        tryToFetch();
    }

    /* --- Post-evaluate --- */
    postEvaluate(line);
}

} // namespace minor
} // namespace gem5
