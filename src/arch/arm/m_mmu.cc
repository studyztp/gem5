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
#include "cpu/base.hh"
#include "cpu/thread_context.hh"
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
// QueuedRequestPort with automatic request queuing and retry via
// ReqPacketQueue.  Requests are scheduled with 1-cycle spacing
// to model the AHB pipelining of the Cortex-M stacking sequencer.
// Only recvTimingResp() is needed — retry is handled by the queue.

MMMU::StackingPort::StackingPort(const std::string &name, MMMU &mmu)
    : QueuedRequestPort(name, reqPktQueue, snoopRespQueue),
      mmu(mmu),
      reqPktQueue(mmu, *this),
      snoopRespQueue(mmu, *this)
{
}

bool
MMMU::StackingPort::recvTimingResp(PacketPtr pkt)
{
    // Each response corresponds to one stacking write or unstacking
    // read completing in the memory system.  Decrement the counter
    // and release the barrier when all responses have arrived.
    assert(mmu.pendingResponses > 0);
    --mmu.pendingResponses;

    delete pkt;

    if (mmu.pendingResponses == 0)
        mmu.stackingComplete();

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
    // All stacking writes or unstacking reads have completed in
    // the memory system.  Release the barrier so held icache
    // responses are forwarded to the CPU, allowing it to proceed
    // with executing the exception handler (or returning from one).
    if (barrier)
        barrier->releaseAll();
}

// -- storeToStack --

void
MMMU::storeToStack(Addr frameptr, const std::vector<uint32_t> &values,
                   ThreadContext *tc)
{
    RequestorID rid = stackingRequestorId;
    bool isTimingMode = tc->getSystemPtr()->isTimingMode();

    for (uint32_t i = 0; i < values.size(); ++i) {
        Addr addr = frameptr + i * 4;

        auto req = std::make_shared<Request>(addr, 4,
            Request::UNCACHEABLE, rid);
        auto pkt = new Packet(req, MemCmd::WriteReq);
        pkt->allocate();
        pkt->setLE<uint32_t>(values[i]);

        if (isTimingMode) {
            // Timing mode: schedule writes with 1-cycle spacing,
            // modeling AHB pipelining of the stacking sequencer.
            // The stacking barrier holds icache responses until
            // all writes complete.
            if (i == 0 && barrier)
                barrier->startHolding();
            Tick cpuClkPeriod = tc->getCpuPtr()->clockPeriod();
            Tick sendTick = curTick() + i * cpuClkPeriod;
            stackingPort.schedTimingReq(pkt, sendTick);
        } else {
            // Atomic mode: send immediately, get latency back.
            // No barrier needed — AtomicSimpleCPU doesn't use
            // timing responses for stall.
            stackingPort.sendAtomic(pkt);
            delete pkt;
        }
    }

    if (isTimingMode)
        pendingResponses = values.size();
}

// -- readFromStack --

std::vector<uint32_t>
MMMU::readFromStack(Addr frameptr, uint32_t numWords, ThreadContext *tc)
{
    std::vector<uint32_t> result(numWords);

    // Phase 1: Functional reads for immediate data.
    // The caller (mProfileExcReturnUnstack) needs the register
    // values immediately to restore CPU state.  sendFunctional()
    // reads directly from the backing store without going through
    // the timing/atomic protocol — valid in any memory mode.
    for (uint32_t i = 0; i < numWords; ++i) {
        Addr addr = frameptr + i * 4;

        auto req = std::make_shared<Request>(addr, 4,
            Request::UNCACHEABLE, Request::funcRequestorId);
        auto pkt = new Packet(req, MemCmd::ReadReq);
        pkt->allocate();

        stackingPort.sendFunctional(pkt);
        result[i] = pkt->getLE<uint32_t>();

        delete pkt;
    }

    // Phase 2: Timing reads for realistic latency (timing mode only).
    // The actual data is already captured above.  These timing
    // requests model the bus cycles the unstacking would take on
    // real hardware.  The barrier holds the CPU until all timing
    // responses arrive.  Skipped in atomic mode — no timing protocol.
    if (tc->getSystemPtr()->isTimingMode()) {
        if (barrier)
            barrier->startHolding();

        pendingResponses = numWords;

        Tick cpuClkPeriod = tc->getCpuPtr()->clockPeriod();
        RequestorID rid = stackingRequestorId;

        for (uint32_t i = 0; i < numWords; ++i) {
            Addr addr = frameptr + i * 4;

            auto req = std::make_shared<Request>(addr, 4,
                Request::UNCACHEABLE, rid);
            auto pkt = new Packet(req, MemCmd::ReadReq);
            pkt->allocate();

            Tick sendTick = curTick() + i * cpuClkPeriod;
            stackingPort.schedTimingReq(pkt, sendTick);
        }
    }

    return result;
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
    if (!barrier.memSidePort.sendTimingReq(pkt)) {
        // Memory side is busy — need to retry later.
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
        barrier.heldResponses.push_back(pkt);
        return true;
    }

    // Normal operation: forward response to CPU immediately.
    if (!barrier.cpuSidePort.sendTimingResp(pkt)) {
        // CPU side is busy — remember to retry.
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
    holdResponses = true;
}

void
StackingBarrier::releaseAll()
{
    holdResponses = false;

    // Forward all held responses to the CPU in FIFO order.
    // On real hardware, stacking completes and the instruction
    // fetch response (which arrived during stacking) is delivered
    // to the pipeline.
    while (!heldResponses.empty()) {
        PacketPtr pkt = heldResponses.front();
        heldResponses.pop_front();

        if (!cpuSidePort.sendTimingResp(pkt)) {
            // CPU side rejected — put it back and wait for retry.
            heldResponses.push_front(pkt);
            retryResp = true;
            break;
        }
    }
}

} // namespace ArmISA
} // namespace gem5
