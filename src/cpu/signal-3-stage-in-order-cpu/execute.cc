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

#include "cpu/signal-3-stage-in-order-cpu/execute.hh"

#include "arch/generic/pcstate.hh"
#include "base/logging.hh"
#include "cpu/signal-3-stage-in-order-cpu/exec_context.hh"
#include "cpu/signal-3-stage-in-order-cpu/signal_cpu.hh"
#include "cpu/static_inst.hh"
#include "cpu/thread_context.hh"
#include "debug/Signal3CPUExecute.hh"
#include "sim/faults.hh"

namespace gem5
{
namespace signal3
{

Execute::Execute(SignalGraph &g, SignalCPU &cpu, Decode &decode,
                 LSQ &lsq)
    : Stage(g, "execute", /*stageId=*/2),
      e_accept_d(g, "e_accept_d_out", /*fromStage=*/this,
                 /*pulse=*/false),
      e_redirect_to_f(g, "e_redirect_to_f", /*fromStage=*/this,
                      /*pulse=*/true),
      e_flagSetterNextPc(g, "e_flagSetterNextPc",
                         /*fromStage=*/this, /*pulse=*/true),
      _cpu(cpu),
      _decode(decode),
      _lsq(lsq)
{
    // Re-fire when D presents a new slot or when an LSQ response
    // lands.  Subscribing to D's dSlot.out() means E re-evaluates
    // any time D writes a new latch input AND the latch commits
    // (changing the output value).
    _decode.dSlot.out().subscribe(this);
    _lsq.completionSignal.subscribe(this);
}

void
Execute::resetTo(Addr /*entryPc*/)
{
    _eSlot.reset();
    _committedThisCycle = false;
}

Addr
Execute::archPc() const
{
    return _cpu.thread(0)->getTC()->pcState().instAddr();
}

void
Execute::settle()
{
    // ----------------------------------------------------------------
    // 1. Drive back-pressure to D.
    // E accepts a new slot iff the LSQ is Idle (no pending memory
    // op) and either there's no in-flight slot or we're going to
    // commit it this cycle.  We compute conservatively first; the
    // value may be tightened later in this settle pass.
    // ----------------------------------------------------------------
    const bool lsqStallsAccept = _lsq.pending();
    const bool slotBlocksAccept = _eSlot.has_value()
                                    && !_lsq.complete();
    e_accept_d.write(!lsqStallsAccept && !slotBlocksAccept);

    if (_committedThisCycle)
        return;

    // ----------------------------------------------------------------
    // 2. If the LSQ is Pending, the in-flight slot is a memref that
    // hasn't responded — stall until completionSignal fires.
    // ----------------------------------------------------------------
    if (_lsq.pending()) {
        _cpu.signalStats().memStallCycles++;
        DPRINTF(Signal3CPUExecute, "stall: LSQ pending\n");
        return;
    }

    // ----------------------------------------------------------------
    // 3. Pull a new slot from D's dSlot.out() if E is empty.
    //    dSlot.out() reflects the value latched at this cycle's Edge.
    // ----------------------------------------------------------------
    if (!_eSlot.has_value()) {
        if (_decode.dSlot.out().valid()
                && _decode.dSlot.out().read().has_value()) {
            _eSlot = _decode.dSlot.out().read();
            DPRINTF(Signal3CPUExecute,
                    "accept slot pc=%#x\n", _eSlot->addr);
        }
    }

    if (!_eSlot.has_value())
        return;

    // ----------------------------------------------------------------
    // 4. Commit the in-flight slot.  Mirrors Simple3CycleCPU's
    //    Execute::runOneCycle 4-path dispatch.
    // ----------------------------------------------------------------
    commitOne();
}

void
Execute::commitOne()
{
    DecodedSlot &e = *_eSlot;
    SimpleThread &thread = *_cpu.thread(0);
    panic_if(!e.pcStateAtDecode,
             "Execute: DecodedSlot missing pcStateAtDecode");

    // ---- earlyResolved fast path (S5+; dead in S3/S4) --------------
    if (e.earlyResolved) {
        std::unique_ptr<PCStateBase> newPc(thread.pcState().clone());
        newPc->set(e.resolvedNpc);
        thread.pcState(*newPc);
        DPRINTF(Signal3CPUExecute,
                "commit (early-resolved) pc=%#x nextPc=%#x\n",
                e.addr, e.resolvedNpc);
        _cpu.signalStats().instsCommitted++;
        _eSlot.reset();
        _committedThisCycle = true;
        // Re-publish e_accept_d now that the slot is gone.
        e_accept_d.write(!_lsq.pending());
        return;
    }

    thread.pcState(*e.pcStateAtDecode);
    ExecContext ctx(_cpu, thread);
    Fault fault = NoFault;

    if (_lsq.complete()) {
        // Memory inst whose response just arrived.
        PacketPtr pkt = _lsq.completedPacket();
        DPRINTF(Signal3CPUExecute,
                "completeAcc pc=%#x cmd=%s\n",
                e.addr, pkt->cmdString());
        fault = e.staticInst->completeAcc(pkt, &ctx,
                                          /*traceData=*/nullptr);
        _lsq.release();
    } else if (e.staticInst->isMemRef()) {
        // First touch of a memref: issue via ExecContext (which
        // pushes through the LSQ).  Stall if the LSQ went Pending.
        DPRINTF(Signal3CPUExecute,
                "initiateAcc pc=%#x size=%u\n",
                e.addr, e.staticInst->size());
        fault = e.staticInst->initiateAcc(&ctx,
                                          /*traceData=*/nullptr);
        panic_if(fault != NoFault,
                 "Execute: fault from initiateAcc(): %s",
                 fault->name());
        if (_lsq.pending()) {
            DPRINTF(Signal3CPUExecute,
                    "memref issued, awaiting response\n");
            // Don't commit — wait for completionSignal next re-fire.
            // Republish back-pressure: now that LSQ is Pending, we
            // can't accept a new slot.
            e_accept_d.write(false);
            return;
        }
        // initiateAcc didn't push (predicate failed); fall through.
    } else {
        // Non-memory inst: full execute in one cycle.
        fault = e.staticInst->execute(&ctx, /*traceData=*/nullptr);
        panic_if(fault != NoFault,
                 "Execute: fault from execute(): %s",
                 fault->name());
    }

    // Retire.
    _cpu.signalStats().instsCommitted++;
    e.staticInst->advancePC(thread.getTC());
    const Addr actualNextPc = thread.getTC()->pcState().instAddr();

    // Any commit whose nextPc differs from the sequential fall-through
    // must redirect F — regardless of whether the staticInst reports
    // isControl().  Cortex-M's `bx lr` with an EXC_RETURN value
    // triggers the unstacking sequence inside execute(), updating the
    // thread's PC to the saved return address; gem5's ARM ISA does
    // NOT always mark this instruction as isControl(), so we'd miss
    // the redirect if we gated on that flag.  The address-only check
    // catches both architecturally-classified branches and
    // exception-return PC updates uniformly.
    const Addr fallThrough = e.addr + e.staticInst->size();
    const bool needRedirect = (actualNextPc != fallThrough);

    DPRINTF(Signal3CPUExecute,
            "commit pc=%#x size=%u nextPc=%#x ctrl=%d redirect=%d\n",
            e.addr, e.staticInst->size(), actualNextPc,
            (int)e.staticInst->isControl(), (int)needRedirect);

    if (needRedirect) {
        e_redirect_to_f.write(std::optional<Addr>{actualNextPc});
        _cpu.signalStats().mispredicts++;
    }

    // S5: pulse the flag-setter forward signal so D can resolve
    // a dependent conditional branch this same cycle.  We pulse
    // for every non-control commit (whether the inst actually wrote
    // NZCV or not).  D's speculation is safe in either case
    // because the inst between this commit and the branch's E
    // execution is the branch itself (in-order single-issue), so
    // tc.NZCV at D-speculation time is the same value it would have
    // at E-execution time.
    if (!e.staticInst->isControl()) {
        const Addr fallThrough2 = e.addr + e.staticInst->size();
        e_flagSetterNextPc.write(std::optional<Addr>{fallThrough2});
    }

    _eSlot.reset();
    _committedThisCycle = true;
    // After retiring, E can accept a new slot this same cycle if the
    // LSQ is Idle.  Tighten back-pressure accordingly.
    e_accept_d.write(!_lsq.pending());
}

} // namespace signal3
} // namespace gem5
