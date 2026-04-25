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

#include "cpu/simple-3-cycle-in-order-cpu/pipeline.hh"

#include <memory>

#include "arch/generic/pcstate.hh"
#include "base/logging.hh"
#include "cpu/simple-3-cycle-in-order-cpu/exec_context.hh"
#include "cpu/simple-3-cycle-in-order-cpu/simple_3cycle_cpu.hh"
#include "cpu/static_inst.hh"
#include "cpu/thread_context.hh"
#include "debug/Simple3CPU.hh"
#include "debug/Simple3CPUExecute.hh"
#include "debug/Simple3CPULineTrace.hh"
#include "sim/faults.hh"
#include "sim/sim_exit.hh"

namespace gem5
{
namespace simple3
{

Pipeline::Pipeline(Simple3CycleCPU &cpu_)
    : Ticked(cpu_, nullptr, Event::CPU_Tick_Pri),
      cpu(cpu_)
{
    pfu = std::make_unique<PFU>(cpu, cpu.params().pfu_fifo_words);
    decode = std::make_unique<Decode>(cpu);
    execute = std::make_unique<Execute>(cpu);
    // PFU's fetchPc is set in startup() once the test config (Phase
    // B / Phase C / future Phase D) tells us where to start fetching.
    pfu->reset(0);
}

void
Pipeline::resetForPhaseC(Addr entryPc)
{
    pfu->reset(entryPc);
    decode->setNextInstrAddrToDeliver(entryPc);
    decode->resetFetchTracking();
}

void
Pipeline::prepopulateFifoForPhaseB(Addr startAddr, unsigned count,
                                   uint32_t wordValue)
{
    pfu->reset(startAddr);
    decode->setNextInstrAddrToDeliver(startAddr);
    decode->resetFetchTracking();
    for (unsigned i = 0; i < count; ++i) {
        FetchWord w;
        w.addr = startAddr + i * sizeof(uint32_t);
        w.data = wordValue;
        w.streamId = cpu.currentStreamId;
        pfu->deliverWord(w);
    }
}

void
Pipeline::evaluate()
{
    ThreadContext *tc = cpu.thread(0)->getTC();

    // 0. Drain any icache responses that arrived since the last cycle
    //    edge into the FIFO (Python R5 ordering: F service runs
    //    before D consumes).
    pfu->drainResponseStaging();
    pfu->tickRedirectDelay();

    // 1. Execute commits whatever is in eSlot.  This is the cycle
    //    in which any flag-setting inst makes its CPSR/xPSR update
    //    visible — the flags become available *this* cycle for the
    //    M-profile E→D forwarding mechanism.
    MispredictInfo mp = execute->runOneCycle(tc);

    // 2. Mispredict (legacy path).  With E→D forwarding doing the
    //    work in step 2.5 below, this path should fire only when an
    //    early-resolve was impossible (e.g. an indirect branch like
    //    BX/BLX that we don't pre-execute) and the prediction was
    //    therefore implicitly "not taken / fall through".  Kept as
    //    a correctness safety net.
    if (mp.happened) {
        decode->squashDSlot();
        decode->setNextInstrAddrToDeliver(mp.actualPc);
        pfu->filterFifoAndRedirect(mp.actualPc);
        decode->startFlushBubble(
            Cycles(cpu.params().mispredict_flush_cycles));
        cpu.simple3Stats.mispredicts++;
        cpu.currentStreamId++;
    }

    // 2.5. Cortex-M4 E→D forwarding — early branch resolution.
    //      If dSlot holds a *direct* branch (no register-based target)
    //      we pre-run its execute() now using the flags step 1 just
    //      committed, and redirect F immediately if taken.  See
    //      prototype/stm32g4_no_art.py "early-decode branch
    //      resolution" comment.  Only direct branches are eligible —
    //      register-indirect ones (BX/BLX reg) might depend on a
    //      register the prior inst is still computing, and can't be
    //      resolved here.
    earlyResolveBranchAtD(tc);

    // 3. D -> E (back-pressured: don't overwrite a stalled eSlot,
    //    e.g. an LSQ-pending memref).
    if (!execute->busy() && decode->hasInstrReadyForExecute())
        execute->acceptFromDecode(decode->popForExecute());

    // 4. FIFO -> D (gated by flush bubble).
    if (!decode->isFlushing())
        decode->stepOneInstr(*pfu, tc);

    decode->decrementFlushCounter();
    if (decode->isFlushing())
        cpu.simple3Stats.flushBubbleCycles++;

    // 5. Issue a new icache request if the port isn't holding off a
    //    prior retry and the PFU wants one.  After sendTimingReq()
    //    returns false the gem5 contract requires us to wait for
    //    recvReqRetry — so we don't try again here.
    if (!pendingRetryPacket && pfu->shouldIssueFetch()) {
        PacketPtr pkt = pfu->buildFetchPacket(cpu.currentStreamId,
                                              cpu.instRequestorId());
        trySendFetchPacket(pkt);
    }

    if (debug::Simple3CPULineTrace)
        dumpLineTrace();

    // Halt detection: when the architectural PC (now in
    // tc->pcState() — Phase D wired Execute through real
    // ExecContext) reaches the halt address and the pipeline has
    // drained, stop ticking so numCycles freezes.
    if (cpu.haltAddr() != 0 &&
        tc->pcState().instAddr() == cpu.haltAddr() &&
        !execute->busy() && !decode->busy()) {
        DPRINTF(Simple3CPU,
                "halt: pc=%#x reached, pipeline drained, "
                "stopping at cycle=%llu\n",
                cpu.haltAddr(),
                (unsigned long long)numCycles.value());
        stop();
        // Schedule a sim-loop exit so m5.simulate() returns
        // promptly; without this it runs to MaxTick draining
        // any in-flight memory responses.  Exit code 0 = clean halt.
        exitSimLoop("Simple3CycleCPU reached haltAddr", 0);
    } else if (cpu.haltAddr() == 0) {
        // No halt configured (vacuous test): stop after one cycle.
        stop();
        exitSimLoop("Simple3CycleCPU vacuous (no haltAddr)", 0);
    }
}

void
Pipeline::earlyResolveBranchAtD(ThreadContext *tc)
{
    // Need an inst in dSlot, and it has to be a direct branch we
    // haven't already resolved.
    if (!decode->slot())
        return;
    DecodedSlotEntry &de = const_cast<DecodedSlotEntry &>(*decode->slot());
    if (de.earlyResolved)
        return;
    StaticInstPtr si = de.staticInst;
    if (!si->isControl() || si->isIndirectCtrl())
        return;
    panic_if(!de.pcStateAtDecode,
             "earlyResolveBranchAtD: missing pcStateAtDecode");

    // Pre-run the branch's execute() against the just-committed
    // flags.  Save the thread's pcState first so we can restore it —
    // we mustn't clobber the architectural PC mid-cycle (the bne
    // hasn't actually retired yet, only F's redirect is happening
    // now).
    SimpleThread &thread = *cpu.thread(0);
    std::unique_ptr<PCStateBase> savedPc(thread.pcState().clone());

    thread.pcState(*de.pcStateAtDecode);
    ExecContext ctx(cpu, thread);
    Fault f = si->execute(&ctx, /*traceData=*/nullptr);
    panic_if(f != NoFault,
             "earlyResolveBranchAtD: fault from speculative "
             "execute(): %s", f->name());
    si->advancePC(tc);
    const Addr resolvedNpc = thread.pcState().instAddr();

    // Restore — Execute will commit the inst when it reaches E next
    // cycle, advancing the architectural PC at that point.
    thread.pcState(*savedPc);

    // Stash on the slot so Execute knows it's pre-resolved.
    decode->markSlotEarlyResolved(resolvedNpc);

    const Addr fallThrough = de.addr + si->size();
    DPRINTF(Simple3CPUExecute,
            "early-resolve at D: pc=%#x size=%u resolvedNpc=%#x "
            "fallThrough=%#x taken=%d\n",
            de.addr, si->size(), resolvedNpc, fallThrough,
            (int)(resolvedNpc != fallThrough));

    if (resolvedNpc != fallThrough) {
        // Per verify_model.py's classification (mirrors Cortex-M4
        // empirical behaviour):
        //
        //   - Backward taken: Cortex-M4 resolves at D, drains the
        //     in-flight fall-through stream, and re-fetches the
        //     target line.  Always pays the full TAKEN_BRANCH
        //     bundle (~10 HCLK).  We model this with UNCACHEABLE on
        //     the next fetch + `taken_branch_redirect_delay` HCLK
        //     of F hold-off.
        //
        //   - Forward taken: silicon has speculatively fetched the
        //     fall-through line, so if the target is *in that same
        //     line*, no Flash access is needed — the buffer already
        //     has the bytes.  Cheap "MISPREDICT_SAME_LINE" bubble
        //     (~2 HCLK).  If target is on a different line, full
        //     bundle (10 HCLK) like the backward case.
        //
        // The `forward && sameLine` case is what nested-loop
        // benchmarks like alternating need to match Python.
        const Addr LINE_MASK = ~Addr(0x7);
        const bool isForward = resolvedNpc > de.addr;
        const bool sameLine =
            (resolvedNpc & LINE_MASK) == (fallThrough & LINE_MASK);
        const bool cheapRedirect = isForward && sameLine;

        pfu->staticPredictRedirect(resolvedNpc, cheapRedirect);
        if (!cheapRedirect) {
            pfu->setRedirectFetchDelay(
                cpu.params().taken_branch_redirect_delay);
        }
        decode->resetFetchTracking();
        decode->setNextInstrAddrToDeliver(resolvedNpc);
        // Bump streamId for full-bundle redirects so wrong-path
        // in-flight fetches are dropped on arrival.  For
        // cheap-fwd-sameline, the in-flight speculatively-fetched
        // words are AHEAD of fall-through, which is past the target
        // (target is between fall-through and the in-flight) — they
        // include the target word, so KEEP them by leaving
        // currentStreamId alone.  Without this skip, the cheap
        // redirect re-fetches the target word and pays an extra
        // 1-2 HCLK we don't need.
        if (!cheapRedirect)
            cpu.currentStreamId++;

        DPRINTF(Simple3CPUExecute,
                "  redirect kind=%s (forward=%d sameLine=%d)\n",
                cheapRedirect ? "cheap-fwd-sameline" : "full-bundle",
                (int)isForward, (int)sameLine);
    }
}

bool
Pipeline::trySendFetchPacket(PacketPtr pkt)
{
    if (cpu.getIcachePort().sendTimingReq(pkt)) {
        pfu->noteIssued();
        cpu.simple3Stats.requestsIssued++;
        if (pendingRetryPacket == pkt)
            pendingRetryPacket = nullptr;
        return true;
    }
    pendingRetryPacket = pkt;
    return false;
}

void
Pipeline::retrySendPendingFetch()
{
    if (!pendingRetryPacket)
        return;
    trySendFetchPacket(pendingRetryPacket);
}

void
Pipeline::dumpLineTrace()
{
    // One-line-per-cycle trace, to be filled in to mirror the Python
    // prototype's column layout once the stages produce real state.
    DPRINTF(Simple3CPULineTrace, "cyc=%llu\n",
            (unsigned long long)numCycles.value());
}

} // namespace simple3
} // namespace gem5
