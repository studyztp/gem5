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

#include "cpu/signal-3-stage-in-order-cpu/pipeline.hh"

#include "arch/arm/m_interrupts.hh"
#include "arch/arm/pcstate.hh"
#include "arch/arm/regs/misc.hh"
#include "arch/arm/regs/misc_types.hh"
#include "arch/generic/decoder.hh"
#include "arch/generic/interrupts.hh"
#include "base/logging.hh"
#include "cpu/signal-3-stage-in-order-cpu/signal_cpu.hh"
#include "cpu/thread_context.hh"
#include "debug/Signal3CPU.hh"
#include "debug/Signal3CPULineTrace.hh"
#include "debug/Signal3CPUMem.hh"
#include "debug/Signal3CPUPipeline.hh"
#include "sim/faults.hh"
#include "sim/full_system.hh"
#include "sim/sim_exit.hh"

namespace gem5
{
namespace signal3
{

Pipeline::Pipeline(SignalCPU &cpu, unsigned fifoCapacity)
    : Ticked(cpu),
      _cpu(cpu),
      _graph()
{
    _lsq = std::make_unique<LSQ>(_graph, _cpu);
    _fetch = std::make_unique<Fetch>(_graph, _cpu, fifoCapacity);
    _decode = std::make_unique<Decode>(_graph, _cpu, *_fetch);
    _execute = std::make_unique<Execute>(_graph, _cpu, *_decode, *_lsq);

    // F <- D's pop pulse.
    _fetch->setPopRequestInput(&_decode->d_pop_request);
    _decode->d_pop_request.subscribe(_fetch.get());

    // D <- E's accept signal: D peeks via pointer; E's signal is
    // subscribed-to by D so D re-fires when back-pressure changes.
    _decode->setEAcceptInput(&_execute->e_accept_d);
    _execute->e_accept_d.subscribe(_decode.get());

    // F <- E's redirect signal (taken-branch from indirect or
    // anything resolved at E).  D also subscribes so it can drop
    // the speculative dSlot and reset its decoder cache when E
    // redirects.
    _fetch->setERedirectInput(&_execute->e_redirect_to_f);
    _decode->setERedirectInput(&_execute->e_redirect_to_f);
    _execute->e_redirect_to_f.subscribe(_fetch.get());
    _execute->e_redirect_to_f.subscribe(_decode.get());

    // S5: F <- D's same-cycle redirect (early-resolved branches).
    //     D <- E's flag-setter pulse so D re-fires after E commits
    //     a flag-producing inst and can resolve its dependent
    //     conditional branch the same cycle.
    _fetch->setDRedirectInput(&_decode->d_redirect_to_f);
    _decode->setEFlagSetterInput(&_execute->e_flagSetterNextPc);
    _decode->d_redirect_to_f.subscribe(_fetch.get());
    _execute->e_flagSetterNextPc.subscribe(_decode.get());
}

void
Pipeline::resetTo(Addr entryPc)
{
    _fetch->resetTo(entryPc);
    _decode->resetTo(entryPc);
}

void
Pipeline::resetSettleGuards()
{
    // Re-arm one-shot per-cycle guards in stages.  This is a thin
    // hook that's easy to extend once D/E exist; for now we simply
    // poke into Fetch via a friend-style accessor by re-running
    // resetTo's drain-flag clear.  Cleaner approach: each Stage has
    // a virtual onBeginCycle() — added in S2.
    //
    // For S1, we exploit the fact that Fetch::settle()'s drain guard
    // resets via reseed at construction; here we hand-clear it by
    // briefly accessing the field through a friend.  Given there's
    // no D yet, the only externally visible effect of this guard is
    // the absorbStaging dedup, which is correct as-is for S1.
}

void
Pipeline::checkExcReturn()
{
    if (!FullSystem)
        return;
    // Only safe at instruction boundaries (no half-committed memref).
    if (!_lsq->idle())
        return;
    if (_execute->busy())
        return;

    SimpleThread *t = _cpu.thread(0);
    ThreadContext *tc = t->getTC();
    const Addr archPc = tc->pcState().instAddr();
    if ((archPc & 0xFFFFFFF0) != 0xFFFFFFF0)
        return;

    DPRINTF(Signal3CPU,
            "EXC_RETURN detected: archPc=%#x, dispatching to "
            "MProfileInterrupts::excReturn\n", archPc);

    auto *mintr = dynamic_cast<ArmISA::MProfileInterrupts *>(
        _cpu.getInterruptController(0));
    panic_if(!mintr, "M-profile interrupt controller missing");
    mintr->excReturn(tc, (uint32_t)archPc);

    // excReturn set _npc to the saved return address.  Advance _pc to
    // _npc so the next fetch comes from there (mirrors m_mmu.cc:94).
    auto pc = tc->pcState().as<ArmISA::PCState>();
    pc.advance();
    tc->pcState(pc);

    const Addr returnPc = tc->pcState().instAddr();
    DPRINTF(Signal3CPU, "EXC_RETURN restored archPc=%#x\n", returnPc);

    // Flush our pipeline like a regular redirect: clear FIFO,
    // drop dSlot, etc.
    tc->getDecoderPtr()->reset();
    _fetch->applyRedirect(returnPc);
    _decode->applyRedirect(returnPc);
    _execute->resetTo(returnPc);
}

void
Pipeline::checkAndTakeInterrupts()
{
    // Only meaningful in FullSystem (interrupts vector is empty in SE).
    if (!FullSystem)
        return;
    if (!_cpu.checkInterrupts(0))
        return;
    // Defer if a memref is in flight: the half-completed inst would
    // be re-issued on exception return, which the architectural
    // model can handle but is simpler to avoid.  Also defer if E
    // has a slot mid-commit (waiting on LSQ).  Cortex-M does
    // late-arriving / tail-chaining; we model the simpler
    // "wait for safe point" approach.
    if (!_lsq->idle())
        return;
    if (_execute->busy())
        return;

    auto *intr = _cpu.getInterruptController(0);
    Fault fault = intr->getInterrupt();
    if (fault == NoFault)
        return;

    SimpleThread *t = _cpu.thread(0);
    ThreadContext *tc = t->getTC();

    DPRINTF(Signal3CPU,
            "taking interrupt %s (pc was %#x)\n",
            fault->name(), tc->pcState().instAddr());

    intr->updateIntrInfo();
    fault->invoke(tc);
    // The fault invocation pushed the M-profile stack frame and set
    // tc.pcState() to the vector handler.  Reset the gem5 generic
    // decoder so it doesn't have stale bytes for the pre-IRQ PC.
    tc->getDecoderPtr()->reset();

    // Flush our own pipeline state to match: clear F's FIFO and
    // reset fetch to the new PC; drop D's slot and reset its
    // decoder cache; drop E's slot.  Bumping the streamId in
    // applyRedirect ensures any in-flight wrong-path responses are
    // dropped at recvTimingResp.
    const auto &armPc = tc->pcState().as<ArmISA::PCState>();
    DPRINTF(Signal3CPU,
            "post-invoke pc()=%#x npc()=%#x instAddr()=%#x thumb=%d\n",
            armPc.pc(), armPc.npc(),
            tc->pcState().instAddr(), (int)armPc.thumb());
    const Addr handlerPc = armPc.pc();
    _fetch->applyRedirect(handlerPc);
    _decode->applyRedirect(handlerPc);
    // E::resetTo nukes the eSlot — same effect we want for IRQ entry.
    _execute->resetTo(handlerPc);
}

void
Pipeline::evaluate()
{
    DPRINTF(Signal3CPU, "evaluate cyc=%llu tick=%llu\n",
            (unsigned long long)numCycles.value(),
            (unsigned long long)curTick());

    _graph.beginCycle(curTick(), _cpu.clockPeriod());

    // Per-cycle hooks on stages (re-arm one-shot guards).
    _decode->beginCycle();
    _execute->beginCycleHook();

    _graph.edge();

    // Catch EXC_RETURN — when the architectural PC sits in the
    // 0xFFFFFFF0..0xFFFFFFFF range (loaded by `bx lr` etc.), the
    // M-profile architecture interprets it as exception-return.
    // SignalCPU bypasses MMU translation for fetches, so we must
    // intercept this before F tries to fetch from a bogus address.
    checkExcReturn();

    // Take any pending interrupt after the latch edge but before
    // Settle.  This way the interrupt sees the "stable" state at
    // the cycle boundary and can flush F/D/E to the vector handler
    // before the new cycle's combinational work begins.
    checkAndTakeInterrupts();

    _graph.settle();
    scheduleOutgoing();

    // Stats: track Settle iteration count if/when the SignalGraph
    // exposes it.  Placeholder for S2+.

    DPRINTF(Signal3CPULineTrace,
            "cyc=%llu F={pc=%#x fifo=%u inflt=%u pend=%lu} "
            "D={next=%#x slotValid=%d}\n",
            (unsigned long long)numCycles.value(),
            _fetch->fetchPc(), _fetch->fifoSize(),
            _fetch->inFlight(),
            _fetch->hasPendingIssue() ? 1ul : 0ul,
            _decode->nextInstrAddrToDeliver(),
            (int)(_decode->dSlot.out().valid()
                  && _decode->dSlot.out().read().has_value()));

    if (haltConditionMet()) {
        DPRINTF(Signal3CPU,
                "halt condition met at cyc=%llu (fetchPc=%#x "
                "halt=%#x fifo=%u in_flight=%u)\n",
                (unsigned long long)numCycles.value(),
                _fetch->fetchPc(), _cpu.haltAddr(),
                _fetch->fifoSize(), _fetch->inFlight());
        stop();
        exitSimLoop("signal-cpu-halt", 0);
    }
}

bool
Pipeline::haltConditionMet() const
{
    if (_cpu.haltAddr() == 0)
        return false;

    // Don't halt inside an exception handler.  M-profile handler
    // addresses live above haltAddr, so the >= check below would
    // false-trigger mid-IRQ.  xPSR.exception is non-zero in handler
    // mode (DDI0403E B1.4.2).
    ThreadContext *tc = _cpu.thread(0)->getTC();
    auto xpsr = tc->readMiscRegNoEffect(ArmISA::MISCREG_M_XPSR);
    const bool inHandler = (xpsr & 0x1FF) != 0;
    if (inHandler)
        return false;

    // E-driven halt: the architectural PC equals halt_addr (the
    // binary's `halt: b halt` infinite loop entry).  The inHandler
    // check above prevents false-triggering mid-IRQ.  No fetch-side
    // gates: F may speculatively fetch past halt_addr (a bunch of
    // `b .` re-fetches), but archPc==halt_addr is the canonical
    // architectural signal.
    return _execute->archPc() == _cpu.haltAddr()
        && !_execute->busy()
        && _lsq->idle();
}

void
Pipeline::scheduleOutgoing()
{
    // The icache port can absorb at most one outstanding request
    // per layer-clear cycle.  If we previously got a refusal, sit
    // tight until recvReqRetry() drains it.
    if (_sawIcacheRetry)
        return;
    if (!_fetch->hasPendingIssue())
        return;
    // AHB-Lite issues one address-phase per clock; mirror that by
    // sending at most one packet per evaluate().  Any further
    // packets queued by Fetch this cycle wait for the next edge.
    if (!_fetch->tryIssuePendingFetch())
        _sawIcacheRetry = true;
}

void
Pipeline::retrySendPendingFetch()
{
    DPRINTF(Signal3CPUMem, "retrySendPendingFetch invoked\n");
    _sawIcacheRetry = false;
    if (!_fetch->hasPendingIssue())
        return;
    if (!_fetch->tryIssuePendingFetch())
        _sawIcacheRetry = true;
}

void
Pipeline::onDcacheResponse(PacketPtr pkt)
{
    const Tick t = curTick();
    const bool inWindow = _graph.isWithinCycle(t);
    DPRINTF(Signal3CPUMem,
            "onDcacheResponse paddr=%#x cmd=%s tick=%llu inWindow=%d\n",
            pkt->getAddr(), pkt->cmdString(),
            (unsigned long long)t, (int)inWindow);
    // The LSQ::completeRequest pulses the completionSignal which
    // re-fires Execute.  If we're inside the cycle window, run
    // Settle now so the cascade propagates this same cycle.
    _lsq->completeRequest(pkt);
    if (inWindow && !_graph.inSettle()) {
        _graph.settle();
        scheduleOutgoing();
    }
}

void
Pipeline::retrySendPendingDcache()
{
    DPRINTF(Signal3CPUMem, "retrySendPendingDcache invoked\n");
    _lsq->retrySend();
}

void
Pipeline::onAsyncResponse(const FetchWord &w, PortSide side)
{
    if (side != PortSide::Icache) {
        // S1: only icache exists.  S3 adds Dcache routing here.
        warn("onAsyncResponse for unsupported PortSide in S1\n");
        return;
    }
    const Tick t = curTick();
    const bool inWindow = _graph.isWithinCycle(t);
    DPRINTF(Signal3CPUMem,
            "onAsyncResponse icache addr=%#x data=%#x streamId=%u "
            "tick=%llu inWindow=%d (cycleStart=%llu period=%llu)\n",
            w.addr, w.data, w.streamId,
            (unsigned long long)t, (int)inWindow,
            (unsigned long long)_graph.cycleStartTick(),
            (unsigned long long)_graph.cyclePeriodTicks());
    if (inWindow) {
        // Mid-cycle delivery — push into F's current-cycle staging
        // and re-Settle so any new word can propagate this same
        // cycle (relevant once D exists; harmless in S1).
        _fetch->recvWordNow(w);
        if (!_graph.inSettle()) {
            _graph.settle();
            scheduleOutgoing();
        }
    } else {
        _fetch->stageForNextCycle(w);
    }
}

} // namespace signal3
} // namespace gem5
