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

#include <sstream>

#include "arch/arm/regs/cc.hh"
#include "arch/generic/pcstate.hh"
#include "base/logging.hh"
#include "cpu/signal-3-stage-in-order-cpu/exec_context.hh"
#include "cpu/signal-3-stage-in-order-cpu/pipeline.hh"
#include "cpu/signal-3-stage-in-order-cpu/signal_cpu.hh"
#include "cpu/static_inst.hh"
#include "cpu/thread_context.hh"
#include "debug/Signal3CPUExecute.hh"
#include "debug/Signal3CPUFlagsFwd.hh"
#include "sim/faults.hh"
#include "sim/insttracer.hh"

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
      e_flags_to_d(g, "e_flags_to_d", /*fromStage=*/this,
                   /*pulse=*/true),
      _cpu(cpu),
      _decode(decode),
      _lsq(lsq),
      _alu(cpu)
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
    // Clear half-committed-macro tracking: the redirect that brought
    // us here invalidated any in-flight macro state.  Whatever the
    // new PC stream is, it starts at an instruction boundary.
    _midMacro = false;
    _alu.reset();
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
            _cpu.pipeline().lineTraceEvent("E.accept");
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
        // PFU stop-at-branch resolution.  Must be called for
        // ANY committed 16-bit control inst, including the
        // early-resolved ones — otherwise F's _branchesInFlight
        // counter never decrements for not-taken cond branches
        // resolved at D (squashStream() only zeroes it on
        // redirect), and F stops fetching forever.  Mirrors the
        // call in the regular commit path below.
        if (e.staticInst && e.staticInst->isControl()
                && e.instSize == 2) {
            _cpu.pipeline().getFetch().noteBranchCommitted();
        }
        // Pulse forwarded flags so D's same-cycle dependent
        // cond-branch sees a valid value on its refire.  The
        // early-resolved fast path covers direct uncond branches
        // (B/BL), which don't modify NZCV, plus decode-only
        // resolved 16-bit T1 Bcond (also non-flag-modifying);
        // the pulse just carries the unchanged flags.
        //
        // ARM keeps NZCV in dedicated CC regs (cc_reg::Nz / C / V),
        // not in MISCREG_CPSR — flag-setting insts write the CC regs
        // directly, so we read from them here.
        {
            ThreadContext *tc = thread.getTC();
            ForwardedFlags ff{
                (uint8_t) tc->getReg(ArmISA::cc_reg::Nz),
                (bool)    tc->getReg(ArmISA::cc_reg::C),
                (bool)    tc->getReg(ArmISA::cc_reg::V)
            };
            DPRINTF(Signal3CPUFlagsFwd,
                    "fwd flags pulse [er] pc=%#x nz=%u c=%u v=%u\n",
                    e.addr, (unsigned)ff.nz, (unsigned)ff.c,
                    (unsigned)ff.v);
            e_flags_to_d.write(ff);
        }
        // Track half-committed-macro state for the IRQ gate.  The
        // early-resolved fast path only fires for direct branches
        // and decode-only resolved cond branches — all non-macro,
        // so this clears _midMacro (a no-op unless a prior macro
        // committed a non-last uop and somehow this insn slipped in
        // before the last uop, which shouldn't happen but is the
        // safe value).
        _midMacro = !e.isLastInMacro;
        _eSlot.reset();
        _committedThisCycle = true;
        // Re-publish e_accept_d now that the slot is gone.
        e_accept_d.write(!_lsq.pending());
        return;
    }

    thread.pcState(*e.pcStateAtDecode);
    ExecContext ctx(_cpu, thread);
    Fault fault = NoFault;

    // The DecodedSlot's staticInst is always a leaf (a non-macro
    // StaticInst or a single micro-op).  D's settle() expands ARM
    // macro-ops into a stream of micro-ops, one DecodedSlot per
    // cycle (Minor-style — see decode.cc).  E never sees a macro-op
    // here, so execute()/initiateAcc() are always safe to call.
    StaticInstPtr inst = e.staticInst;

    // Set up tracer record so the Exec debug flag emits a line per
    // committed inst (used by run_signal_m5op_bench.py to compute
    // precise kernel-only cycle counts excluding the m5op overhead).
    trace::InstRecord *traceData = nullptr;
#if TRACING_ON
    if (auto *tracer = _cpu.getTracer()) {
        traceData = tracer->getInstRecord(
            curTick(), thread.getTC(), inst, thread.pcState(),
            /*macrostaticinst=*/nullptr);
    }
#endif

    if (_lsq.complete()) {
        // Memory inst whose response just arrived.
        PacketPtr pkt = _lsq.completedPacket();
        DPRINTF(Signal3CPUExecute,
                "completeAcc pc=%#x cmd=%s\n",
                e.addr, pkt->cmdString());
        fault = inst->completeAcc(pkt, &ctx, traceData);
        _lsq.release();
    } else if (inst->isMemRef()) {
        // First touch of a memref: issue via ExecContext (which
        // pushes through the LSQ).  Stall if the LSQ went Pending.
        DPRINTF(Signal3CPUExecute,
                "initiateAcc pc=%#x size=%u\n",
                e.addr, (unsigned)e.instSize);
        fault = inst->initiateAcc(&ctx, traceData);
        panic_if(fault != NoFault,
                 "Execute: fault from initiateAcc(): %s",
                 fault->name());
        if (_lsq.pending()) {
            DPRINTF(Signal3CPUExecute,
                    "memref issued, awaiting response\n");
            // Don't commit — wait for completionSignal next re-fire.
            // Drop the unused tracer record (memref will get a fresh
            // one when completionSignal re-enters commitOne).
            if (traceData) {
                delete traceData;
            }
            e_accept_d.write(false);
            return;
        }
        // initiateAcc didn't push (predicate failed); fall through.
    } else if (AluFunctionUnit::accepts(inst)) {
        // Integer-ALU op routed through the ALU function unit.
        // For 1-cy ops the FU completes in the same cycle as issue,
        // so we run inst->execute() and retire immediately
        // (preserving the current nop/mov/add timing).  Multi-cycle
        // ops (UDIV/SDIV per TRM Table 3-1) issue this cycle and
        // schedule a single completion event at clockEdge(lat - 1);
        // E stalls (e_accept_d=false, early return) on subsequent
        // cycles until the event fires and the state becomes
        // Complete.
        if (_alu.idle()) {
            _alu.issue(inst, thread.getTC());
        }
        if (_alu.pending()) {
            DPRINTF(Signal3CPUExecute,
                    "ALU pending pc=%#x — stall\n", e.addr);
            if (traceData) {
                delete traceData;
            }
            e_accept_d.write(false);
            return;
        }
        // FU is Complete this cycle — execute and retire.
        const ThreadContext *_dbg_tc = thread.getTC();
        const RegVal _pre_nz =
            _dbg_tc->getReg(ArmISA::cc_reg::Nz);
        const RegVal _pre_c  =
            _dbg_tc->getReg(ArmISA::cc_reg::C);
        fault = inst->execute(&ctx, traceData);
        panic_if(fault != NoFault,
                 "Execute: ALU fault from execute(): %s",
                 fault->name());
        const RegVal _post_nz =
            _dbg_tc->getReg(ArmISA::cc_reg::Nz);
        const RegVal _post_c  =
            _dbg_tc->getReg(ArmISA::cc_reg::C);
        DPRINTF(Signal3CPUFlagsFwd,
                "ALU exec pc=%#x %s pre nz=%llu c=%llu post nz=%llu c=%llu\n",
                e.addr, inst->getName().c_str(),
                (unsigned long long)_pre_nz,
                (unsigned long long)_pre_c,
                (unsigned long long)_post_nz,
                (unsigned long long)_post_c);
        _alu.release();
    } else {
        // Non-memory, non-ALU inst (control / floating-point /
        // miscellaneous): full execute in one cycle on the legacy
        // direct path.
        fault = inst->execute(&ctx, traceData);
        panic_if(fault != NoFault,
                 "Execute: fault from execute(): %s",
                 fault->name());
    }

    // Emit the trace line (if Exec flag enabled) and free the record.
    if (traceData) {
        traceData->dump();
        delete traceData;
    }

    // Retire.
    _cpu.signalStats().instsCommitted++;
    inst->advancePC(thread.getTC());
    const Addr actualNextPc = thread.getTC()->pcState().instAddr();

    // Redirect detection.  For micro-ops in a macro: a non-last
    // micro-op's advancePC only increments microPC (instAddr stays
    // put), so actualNextPc == e.addr — never a redirect.  Only the
    // last micro-op of a macro (or any non-micro inst) can redirect.
    // We use e.isLastInMacro (set by D) rather than isMicroop() flags
    // on the instruction because some ARM micro-ops are constructed
    // with metadata that makes flag-checking unreliable.
    const bool isLast = e.isLastInMacro;
    const Addr fallThrough = e.addr + e.instSize;
    const bool needRedirect = isLast && (actualNextPc != fallThrough);

    DPRINTF(Signal3CPUExecute,
            "commit pc=%#x size=%u nextPc=%#x ctrl=%d redirect=%d "
            "uop=%d last=%d\n",
            e.addr, (unsigned)e.instSize, actualNextPc,
            (int)inst->isControl(), (int)needRedirect,
            (int)inst->isMicroop(), (int)isLast);

    if (needRedirect) {
        e_redirect_to_f.write(std::optional<Addr>{actualNextPc});
        _cpu.signalStats().mispredicts++;
        _cpu.pipeline().lineTraceEvent(
            inst->isIndirectCtrl() ? "E.redir-indir" : "E.redir");
    }

    _cpu.pipeline().lineTraceEvent(
        inst->isMicroop() && !isLast ? "E.commit-uop" : "E.commit");

    // PFU stop-at-branch resolution: when E commits a 16-bit
    // control inst, F's predecode counter for that branch is
    // decremented.  squashStream() handles the redirect case by
    // clearing the counter wholesale; this is for the no-redirect
    // path (not-taken 16-bit cond-branches).  Restricted to 16-bit
    // because F's predecode only flags 16-bit branches (32-bit
    // Thumb branches share encoding patterns with non-control
    // 32-bit insts and aren't predecoded — see fetch.cc's
    // isLikelyBranch16 / fetchWordContainsBranch comments).
    if (isLast && inst->isControl() && e.instSize == 2) {
        _cpu.pipeline().getFetch().noteBranchCommitted();
    }

    // Forward post-commit NZCV to D.  Pulsed on every commit
    // (not just flag-setters) so that D's same-cycle re-fire on
    // this pulse always observes a valid value; the equality
    // guard suppresses redundant cascades when consecutive
    // commits don't change the flags.
    //
    // ARM keeps NZCV in dedicated CC regs (cc_reg::Nz / C / V),
    // not in MISCREG_CPSR — flag-setting insts write the CC regs
    // directly, so we read from them here.
    {
        ThreadContext *tc = thread.getTC();
        ForwardedFlags ff{
            (uint8_t) tc->getReg(ArmISA::cc_reg::Nz),
            (bool)    tc->getReg(ArmISA::cc_reg::C),
            (bool)    tc->getReg(ArmISA::cc_reg::V)
        };
        DPRINTF(Signal3CPUFlagsFwd,
                "fwd flags pulse pc=%#x nz=%u c=%u v=%u\n",
                e.addr, (unsigned)ff.nz, (unsigned)ff.c, (unsigned)ff.v);
        e_flags_to_d.write(ff);
    }

    // Track half-committed macro state for the Pipeline IRQ gate.
    // Set when we just committed a non-last uop of a macro (more
    // uops still due before the macro is architecturally complete);
    // cleared when we commit the last uop (or any non-macro inst).
    // Pipeline::checkAndTakeInterrupts uses this to defer IRQs while
    // a multi-uop macro is partway through committing — see
    // issues/2026-05-10-pushpop_v2-irq-race/ for the failure mode.
    _midMacro = !isLast;

    _eSlot.reset();
    _committedThisCycle = true;
    e_accept_d.write(!_lsq.pending());
}

std::string
Execute::snapshotString() const
{
    std::ostringstream os;
    const char *lsq_st = _lsq.idle()      ? "Idle"
                        : _lsq.pending()  ? "Pending"
                        : _lsq.complete() ? "Complete"
                                          : "?";
    os << "lsq=" << lsq_st << " slot=";
    if (_eSlot.has_value()) {
        os << "v(0x" << std::hex << _eSlot->addr
           << " sz=" << std::dec << (unsigned)_eSlot->instSize << ")";
    } else {
        os << "empty";
    }
    os << " redir=";
    if (e_redirect_to_f.valid() && e_redirect_to_f.read().has_value()) {
        os << "0x" << std::hex << *e_redirect_to_f.read();
    } else {
        os << "-";
    }
    os << " " << _alu.snapshotString();
    return os.str();
}

} // namespace signal3
} // namespace gem5
