/*
 * Copyright (c) 2026 University of California, Davis and Cornell University
 * All rights reserved
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

#include "arch/arm/m_mmu.hh"

#include "arch/arm/m_faults.hh"
#include "arch/arm/m_interrupts.hh"
#include "arch/arm/page_size.hh"
#include "arch/arm/pcstate.hh"
#include "base/trace.hh"
#include "cpu/base.hh"
#include "cpu/thread_context.hh"
#include "debug/MProfileStacking.hh"
#include "mem/packet_access.hh"
#include "sim/faults.hh"
#include "sim/system.hh"

namespace gem5
{

namespace ArmISA
{

// ---------------------------------------------------------------------------
// MTLB — pass-through TLB (identity translation, no protection)
// ---------------------------------------------------------------------------

Fault
MTLB::translateAtomic(const RequestPtr &req, ThreadContext *tc,
                      BaseMMU::Mode mode)
{
    Addr vaddr = req->getVaddr();

    // EXC_RETURN detection for POP {PC} / LDM {PC}.
    //
    // When an LDM/POP micro-op loads an EXC_RETURN value (0xFFFFFFF_)
    // into PC, the CPU tries to fetch the next instruction from that
    // address.  On real Cortex-M hardware, the bus matrix detects this
    // address range and triggers exception return instead of a memory
    // access.  In gem5, there's no memory at 0xFFFFFFF_ so the fetch
    // would panic.
    //
    // We detect this in the MMU translate path (before the fetch
    // reaches memory) and return an ArmMFault that performs the
    // exception return.  The fault's invoke() calls
    // mProfileExcReturn() which pops the exception frame.
    //
    // DDI0403E B1.5.8: Exception return occurs when PC is loaded
    // with a value where bits[31:4] are all 1s (EXC_RETURN prefix).
    // Valid range: 0xFFFFFFF0–0xFFFFFFFF.  The low 4 bits encode
    // which SP and mode to restore to.
    if (mode == BaseMMU::Execute &&
        (vaddr & 0xFFFFFFF0) == 0xFFFFFFF0) {
        // Exception return: deactivate via interrupt controller, then
        // unstack CPU state.  MProfileInterrupts::excReturn() handles both.
        // excReturn() calls mProfileExcReturnUnstack() which uses
        // pc.npc(retAddr).  In this MMU path, advancePC() is NOT called
        // (the fault path skips curStaticInst->advancePC()), so we must
        // manually advance the PC after excReturn returns.
        auto *mintr = dynamic_cast<MProfileInterrupts *>(
            tc->getCpuPtr()->getInterruptController(tc->threadId()));
        assert(mintr && "M-profile CPU must use MProfileInterrupts");
        mintr->excReturn(tc, (uint32_t)vaddr);
        // excReturn set _npc = retAddr.  Advance _pc = _npc now so
        // that the next fetch (after ReExec::invoke no-op) comes from retAddr,
        // not from 0xFFFFFFF8 again (which would re-trigger this path).
        auto &pc_base = tc->pcState();
        auto pc = pc_base.as<PCState>();
        pc.advance();  // _pc = _npc (retAddr)
        tc->pcState(pc);
        // Return a fault to skip fetchInstMem() and preExecute().
        // ReExec::invoke is a no-op; the CPU re-checks PC on the next tick.
        return std::make_shared<ReExec>();
    }

    // M-profile: VA == PA, no translation.
    // TODO: Add MPU permission checks here when the MPU is modelled.
    req->setPaddr(vaddr);
    return NoFault;
}

void
MTLB::translateTiming(const RequestPtr &req, ThreadContext *tc,
                      BaseMMU::Translation *translation,
                      BaseMMU::Mode mode)
{
    Addr vaddr = req->getVaddr();

    // EXC_RETURN detection — same logic as translateAtomic().
    // Without this, TimingSimpleCPU/MinorCPU panic trying to fetch
    // from 0xFFFFFFF_ (no memory at that address).
    //
    // Timing note: the detection itself is correctly placed (the real
    // bus matrix intercepts this address before it reaches memory, so
    // no icache latency should be charged).  However,
    // mProfileExcReturnUnstack() reads the 8-word exception frame via
    // physProxy (functional port) — zero memory latency.  On real
    // Cortex-M4 this unstacking takes ~12 cycles through the bus
    // matrix → SRAM.  A future fix should route these reads through
    // the timed memory system.  The same inaccuracy exists in
    // translateAtomic() and in ArmMFault::invoke() (frame push).
    if (mode == BaseMMU::Execute &&
        (vaddr & 0xFFFFFFF0) == 0xFFFFFFF0) {
        auto *mintr = dynamic_cast<MProfileInterrupts *>(
            tc->getCpuPtr()->getInterruptController(tc->threadId()));
        assert(mintr && "M-profile CPU must use MProfileInterrupts");
        mintr->excReturn(tc, (uint32_t)vaddr);
        auto &pc_base = tc->pcState();
        auto pc = pc_base.as<PCState>();
        pc.advance();
        tc->pcState(pc);
        translation->finish(std::make_shared<ReExec>(), req, tc, mode);
        return;
    }

    // Pass-through: complete immediately with identity translation.
    req->setPaddr(vaddr);
    translation->finish(NoFault, req, tc, mode);
}

Fault
MTLB::translateFunctional(const RequestPtr &req, ThreadContext *tc,
                          BaseMMU::Mode mode)
{
    req->setPaddr(req->getVaddr());
    return NoFault;
}

Fault
MTLB::finalizePhysical(const RequestPtr &req, ThreadContext *tc,
                       BaseMMU::Mode mode) const
{
    return NoFault;
}

// ---------------------------------------------------------------------------
// MMMU — pass-through MMU + stacking engine
// ---------------------------------------------------------------------------

// -- StackingPort --
//
// Plain RequestPort with a custom priority queue of deferred packets.
// Packets are ordered by scheduled tick (earliest first).  The custom
// queue allows flushStalePackets() to remove abandoned packets directly.

MMMU::StackingPort::StackingPort(const std::string &name, MMMU &mmu)
    : RequestPort(name),
      mmu(mmu),
      sendEvent([this]{ processSendEvent(); }, name + ".sendEvent")
{
}

void
MMMU::StackingPort::schedTimingReq(PacketPtr pkt, Tick when)
{
    pendingQueue.push({when, pkt});
    if (!waitingOnRetry && !sendEvent.scheduled())
        mmu.schedule(sendEvent, std::max(when, curTick() + 1));
}

void
MMMU::StackingPort::processSendEvent()
{
    assert(!pendingQueue.empty());
    auto top = pendingQueue.top();

    if (top.tick > curTick()) {
        mmu.schedule(sendEvent, top.tick);
        return;
    }

    pendingQueue.pop();
    if (!sendTimingReq(top.pkt)) {
        // Rejected — put back and wait for retry.
        pendingQueue.push({curTick(), top.pkt});
        waitingOnRetry = true;
        return;
    }

    if (!pendingQueue.empty() && !sendEvent.scheduled())
        mmu.schedule(sendEvent,
            std::max(pendingQueue.top().tick, curTick() + 1));
}

void
MMMU::StackingPort::recvReqRetry()
{
    waitingOnRetry = false;
    if (!pendingQueue.empty() && !sendEvent.scheduled())
        mmu.schedule(sendEvent,
            std::max(pendingQueue.top().tick, curTick() + 1));
}

bool
MMMU::StackingPort::recvTimingResp(PacketPtr pkt)
{
    // Pop the generation tag from the response packet.
    auto *ss = dynamic_cast<StackingSenderState *>(pkt->popSenderState());
    panic_if(!ss, "StackingPort::recvTimingResp: missing "
             "StackingSenderState on response addr=%#x cmd=%s",
             pkt->getAddr(), pkt->cmdString());
    uint32_t pktGen = ss->generation;
    delete ss;

    DPRINTF(MProfileStacking,
            "StackingPort::recvTimingResp: addr=%#x cmd=%s "
            "pktGen=%u currentGen=%u expected=%u\n",
            pkt->getAddr(), pkt->cmdString(),
            pktGen, mmu.stackingGeneration, mmu.currentGenExpected);

    delete pkt;

    if (pktGen == mmu.stackingGeneration) {
        // Current generation — count toward completion.
        assert(mmu.currentGenExpected > 0);
        --mmu.currentGenExpected;
        if (mmu.currentGenExpected == 0) {
            DPRINTF(MProfileStacking,
                    "StackingPort::recvTimingResp: gen %u complete, "
                    "calling stackingComplete()\n", pktGen);
            mmu.stackingComplete();
        }
    } else {
        // Stale generation — silently ignore.
        DPRINTF(MProfileStacking,
                "StackingPort::recvTimingResp: stale gen %u "
                "(current %u), ignoring\n",
                pktGen, mmu.stackingGeneration);
    }

    return true;
}

// -- MMMU constructor --

MMMU::MMMU(const Params &p)
    : BaseMMU(p),
      stackingPort(p.name + ".stacking_port", *this),
      barrier(p.stacking_barrier),
      stackingRequestorId(p.sys->getRequestorId(this, "stacking"))
{
}

// -- Stacking completion --

void
MMMU::stackingComplete()
{
    DPRINTF(MProfileStacking,
            "stackingComplete: gen=%u\n", stackingGeneration);

    // Notify interrupt controller BEFORE releasing the barrier.
    // This ensures deactivateIRQ() runs before the CPU resumes.
    assert(stackingCompleteCallback);
    stackingCompleteCallback();
    stackingCompleteCallback = nullptr;

    // Release the barrier so held icache responses are forwarded
    // to the CPU, allowing it to proceed.
    if (barrier) {
        DPRINTF(MProfileStacking,
                "stackingComplete: releasing barrier "
                "(holding=%d, heldCount=%u)\n",
                barrier->isHolding(), barrier->numHeld());
        barrier->releaseAll();
    }
}

// -- storeToStack --

void
MMMU::storeToStack(Addr frameptr, const std::vector<uint32_t> &values,
                   ThreadContext *tc)
{
    RequestorID rid = stackingRequestorId;
    bool isTimingMode = tc->getSystemPtr()->isTimingMode();

    if (isTimingMode) {
        // New generation for this stacking operation.
        stackingGeneration++;
        currentGenExpected = values.size();

        // Register with interrupt controller: stacking in progress.
        auto *mintr = dynamic_cast<MProfileInterrupts *>(
            tc->getCpuPtr()->getInterruptController(tc->threadId()));
        assert(mintr);
        mintr->setupStackPending();
        stackingCompleteCallback = [mintr]() {
            mintr->removeStackWritePending();
        };
    }

    DPRINTF(MProfileStacking,
            "storeToStack: frameptr=%#x numWords=%u timing=%d "
            "gen=%u expected=%u\n",
            frameptr, values.size(), isTimingMode,
            stackingGeneration, currentGenExpected);

    for (uint32_t i = 0; i < values.size(); ++i) {
        Addr addr = frameptr + i * 4;

        auto req = std::make_shared<Request>(addr, 4,
            Request::UNCACHEABLE, rid);
        auto pkt = new Packet(req, MemCmd::WriteReq);
        pkt->allocate();
        pkt->setLE<uint32_t>(values[i]);

        if (isTimingMode) {
            if (i == 0 && barrier)
                barrier->startHolding();
            pkt->pushSenderState(
                new StackingSenderState(stackingGeneration));
            Tick cpuClkPeriod = tc->getCpuPtr()->clockPeriod();
            Tick sendTick = curTick() + i * cpuClkPeriod;
            DPRINTF(MProfileStacking,
                    "storeToStack: scheduling write[%u] addr=%#x "
                    "val=%#x at tick %u gen=%u\n",
                    i, addr, values[i], sendTick, stackingGeneration);
            stackingPort.schedTimingReq(pkt, sendTick);
        } else {
            stackingPort.sendAtomic(pkt);
            delete pkt;
        }
    }
}

// -- readFromStack --

std::vector<uint32_t>
MMMU::readFromStack(Addr frameptr, uint32_t numWords, ThreadContext *tc)
{
    std::vector<uint32_t> result(numWords);
    bool isTimingMode = tc->getSystemPtr()->isTimingMode();

    if (isTimingMode) {
        // New generation for this unstacking operation.
        stackingGeneration++;
        currentGenExpected = numWords;

        // Register with interrupt controller: unstacking in progress.
        auto *mintr = dynamic_cast<MProfileInterrupts *>(
            tc->getCpuPtr()->getInterruptController(tc->threadId()));
        assert(mintr);
        mintr->setupStackPending();
        stackingCompleteCallback = [mintr]() {
            mintr->removeStackReadPending();
        };
    }

    DPRINTF(MProfileStacking,
            "readFromStack: frameptr=%#x numWords=%u timing=%d "
            "gen=%u expected=%u\n",
            frameptr, numWords, isTimingMode,
            stackingGeneration, currentGenExpected);

    // Phase 1: Functional reads for immediate data.
    for (uint32_t i = 0; i < numWords; ++i) {
        Addr addr = frameptr + i * 4;

        auto req = std::make_shared<Request>(addr, 4,
            Request::UNCACHEABLE, Request::funcRequestorId);
        auto pkt = new Packet(req, MemCmd::ReadReq);
        pkt->allocate();

        stackingPort.sendFunctional(pkt);
        result[i] = pkt->getLE<uint32_t>();
        DPRINTF(MProfileStacking,
                "readFromStack: functional read[%u] addr=%#x val=%#x\n",
                i, addr, result[i]);

        delete pkt;
    }

    // Phase 2: Timing reads for realistic latency (timing mode only).
    if (isTimingMode) {
        if (barrier)
            barrier->startHolding();

        Tick cpuClkPeriod = tc->getCpuPtr()->clockPeriod();
        RequestorID rid = stackingRequestorId;

        for (uint32_t i = 0; i < numWords; ++i) {
            Addr addr = frameptr + i * 4;

            auto req = std::make_shared<Request>(addr, 4,
                Request::UNCACHEABLE, rid);
            auto pkt = new Packet(req, MemCmd::ReadReq);
            pkt->allocate();

            pkt->pushSenderState(
                new StackingSenderState(stackingGeneration));
            Tick sendTick = curTick() + i * cpuClkPeriod;
            DPRINTF(MProfileStacking,
                    "readFromStack: scheduling read[%u] addr=%#x "
                    "at tick %u gen=%u\n",
                    i, addr, sendTick, stackingGeneration);
            stackingPort.schedTimingReq(pkt, sendTick);
        }
    }

    return result;
}

// -- abandonUnstacking --

void
MMMU::abandonUnstacking()
{
    DPRINTF(MProfileStacking,
            "abandonUnstacking: abandoning gen=%u (expected=%u), "
            "advancing to gen=%u\n",
            stackingGeneration, currentGenExpected,
            stackingGeneration + 1);

    // Advance generation so stale in-flight responses are ignored.
    stackingGeneration++;

    // Flush unsent stale packets from the queue.  Returns the number
    // of packets remaining that match the current generation — these
    // are in-flight and their responses are still expected.
    uint32_t remaining = stackingPort.flushStalePackets(stackingGeneration);
    currentGenExpected = remaining;

    DPRINTF(MProfileStacking,
            "abandonUnstacking: currentGenExpected=%u "
            "(in-flight matching packets)\n", currentGenExpected);

    // Clear the callback — the abandoned operation should not
    // trigger removeStackReadPending when stale responses drain.
    stackingCompleteCallback = nullptr;
}

// -- flushStalePackets --

uint32_t
MMMU::StackingPort::flushStalePackets(uint32_t currentGen)
{
    std::vector<DeferredPacket> keep;
    uint32_t remaining = 0;

    // Pop all packets, keep only those matching currentGen.
    while (!pendingQueue.empty()) {
        auto dp = pendingQueue.top();
        pendingQueue.pop();

        auto *ss = dynamic_cast<StackingSenderState *>(
            dp.pkt->popSenderState());
        panic_if(!ss, "flushStalePackets: missing StackingSenderState "
                 "on pkt addr=%#x", dp.pkt->getAddr());

        if (ss->generation == currentGen) {
            dp.pkt->pushSenderState(ss);
            keep.push_back(dp);
            remaining++;
        } else {
            DPRINTF(MProfileStacking,
                    "flushStalePackets: removing pkt addr=%#x "
                    "gen=%u (current=%u)\n",
                    dp.pkt->getAddr(), ss->generation, currentGen);
            delete ss;
            delete dp.pkt;
        }
    }

    // Re-insert current-gen packets.
    for (auto &dp : keep)
        pendingQueue.push(dp);

    // Deschedule send event if queue is now empty.
    if (pendingQueue.empty() && sendEvent.scheduled())
        mmu.deschedule(sendEvent);

    return remaining;
}

Port &
MMMU::getPort(const std::string &if_name, PortID idx)
{
    if (if_name == "stacking_port")
        return stackingPort;
    else
        return BaseMMU::getPort(if_name, idx);
}

TranslationGenPtr
MMMU::translateFunctional(Addr start, Addr size, ThreadContext *tc,
                          Mode mode, Request::Flags flags)
{
    // Use the standard MMUTranslationGen with the ARM page size.
    // Since MTLB always returns VA == PA, the generator will produce
    // identity-mapped ranges.
    return TranslationGenPtr(new MMUTranslationGen(
            PageBytes, start, size, tc, this, mode, flags));
}

// ---------------------------------------------------------------------------
// StackingBarrier — holds icache responses during exception frame stacking
// ---------------------------------------------------------------------------
//
// On real Cortex-M hardware, exception entry stacking (8-word push to
// SRAM) is performed by a dedicated hardware sequencer on the data bus.
// The instruction bus independently fetches the exception handler
// vector in parallel.  The CPU pipeline does not execute until both
// the stacking and the vector fetch complete.
//
// This barrier models that stall: it sits between the CPU icache port
// and the cache/memory bus.  When stacking starts (startHolding()),
// it lets fetch requests through (so the icache can fill) but queues
// responses.  When stacking finishes (releaseAll()), it forwards the
// queued responses to the CPU, which then proceeds to execute.
//
// Without this, physProxy-based stacking completes in 0 ticks,
// providing no realistic exception entry latency.

// -- Port constructors --

StackingBarrier::CpuSidePort::CpuSidePort(
    const std::string &name, StackingBarrier &barrier)
    : ResponsePort(name), barrier(barrier)
{
}

StackingBarrier::MemSidePort::MemSidePort(
    const std::string &name, StackingBarrier &barrier)
    : RequestPort(name), barrier(barrier)
{
}

// -- StackingBarrier constructor --

StackingBarrier::StackingBarrier(const Params &p)
    : ClockedObject(p),
      cpuSidePort(p.name + ".cpu_side_port", *this),
      memSidePort(p.name + ".mem_side_port", *this)
{
}

Port &
StackingBarrier::getPort(const std::string &if_name, PortID idx)
{
    if (if_name == "cpu_side_port")
        return cpuSidePort;
    else if (if_name == "mem_side_port")
        return memSidePort;
    else
        return ClockedObject::getPort(if_name, idx);
}

// -- CpuSidePort: receives requests from CPU, forwards to memory --

bool
StackingBarrier::CpuSidePort::recvTimingReq(PacketPtr pkt)
{
    // Always forward fetch requests to memory/cache.
    // During stacking, this lets the icache fill in parallel
    // with the stack frame writes on the data bus.
    DPRINTF(MProfileStacking,
            "Barrier::CpuSidePort::recvTimingReq: addr=%#x cmd=%s "
            "holding=%d\n",
            pkt->getAddr(), pkt->cmdString(), barrier.holdResponses);
    if (!barrier.memSidePort.sendTimingReq(pkt)) {
        // Memory side is busy — need to retry later.
        DPRINTF(MProfileStacking,
                "Barrier::CpuSidePort::recvTimingReq: mem side busy, "
                "will retry\n");
        barrier.retryReq = true;
        return false;
    }
    return true;
}

void
StackingBarrier::CpuSidePort::recvRespRetry()
{
    // CPU is ready to accept a response it previously rejected.
    // Try to send the held or pending responses again.
    barrier.retryResp = false;

    // If holding, nothing to do — responses will be sent on release.
    if (barrier.holdResponses)
        return;

    // Not holding but we may have a response that failed to send.
    // releaseAll() handles this case.
}

Tick
StackingBarrier::CpuSidePort::recvAtomic(PacketPtr pkt)
{
    // AtomicSimpleCPU path: pass through directly.
    // No holding in atomic mode — stacking barrier only affects
    // timing mode.  Atomic stacking still uses physProxy (0 latency).
    return barrier.memSidePort.sendAtomic(pkt);
}

void
StackingBarrier::CpuSidePort::recvFunctional(PacketPtr pkt)
{
    // Functional path: pass through directly.
    barrier.memSidePort.sendFunctional(pkt);
}

AddrRangeList
StackingBarrier::CpuSidePort::getAddrRanges() const
{
    // Transparent to address routing — forward ranges from
    // whatever is connected on the memory side.
    return barrier.memSidePort.getAddrRanges();
}

// -- MemSidePort: receives responses from cache/memory --

bool
StackingBarrier::MemSidePort::recvTimingResp(PacketPtr pkt)
{
    // This is the key interception point.
    // If stacking is in progress, queue the response instead of
    // forwarding it to the CPU.  The CPU stalls naturally because
    // it is waiting for the icache response.
    if (barrier.holdResponses) {
        DPRINTF(MProfileStacking,
                "Barrier::recvTimingResp: HOLDING response addr=%#x "
                "cmd=%s (heldCount=%u)\n",
                pkt->getAddr(), pkt->cmdString(),
                barrier.heldResponses.size());
        barrier.heldResponses.push_back(pkt);
        return true;
    }

    DPRINTF(MProfileStacking,
            "Barrier::recvTimingResp: FORWARDING response addr=%#x "
            "cmd=%s to CPU\n",
            pkt->getAddr(), pkt->cmdString());

    // Normal operation: forward response to CPU immediately.
    if (!barrier.cpuSidePort.sendTimingResp(pkt)) {
        // CPU side is busy — remember to retry.
        DPRINTF(MProfileStacking,
                "Barrier::recvTimingResp: CPU rejected response, "
                "will retry\n");
        barrier.retryResp = true;
        return false;
    }
    return true;
}

void
StackingBarrier::MemSidePort::recvReqRetry()
{
    // Memory side is ready to accept a request that previously
    // failed.  Tell the CPU side to retry.
    barrier.retryReq = false;
    barrier.cpuSidePort.sendRetryReq();
}

void
StackingBarrier::MemSidePort::recvRangeChange()
{
    // Forward address range changes to the CPU side so the
    // crossbar can update its routing tables.
    barrier.cpuSidePort.sendRangeChange();
}

// -- Stacking control --

void
StackingBarrier::startHolding()
{
    DPRINTF(MProfileStacking,
            "Barrier::startHolding: begin holding icache responses "
            "(was holding=%d, heldCount=%u)\n",
            holdResponses, heldResponses.size());
    holdResponses = true;
}

void
StackingBarrier::releaseAll()
{
    DPRINTF(MProfileStacking,
            "Barrier::releaseAll: releasing %u held responses\n",
            heldResponses.size());
    holdResponses = false;

    // Forward all held responses to the CPU in FIFO order.
    while (!heldResponses.empty()) {
        PacketPtr pkt = heldResponses.front();
        heldResponses.pop_front();

        DPRINTF(MProfileStacking,
                "Barrier::releaseAll: forwarding held response "
                "addr=%#x cmd=%s (%u remaining)\n",
                pkt->getAddr(), pkt->cmdString(),
                heldResponses.size());

        if (!cpuSidePort.sendTimingResp(pkt)) {
            // CPU side rejected — put it back and wait for retry.
            DPRINTF(MProfileStacking,
                    "Barrier::releaseAll: CPU rejected, "
                    "re-queuing and waiting for retry\n");
            heldResponses.push_front(pkt);
            retryResp = true;
            break;
        }
    }
}

} // namespace ArmISA
} // namespace gem5
