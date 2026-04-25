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

#include "cpu/simple-3-cycle-in-order-cpu/execute.hh"

#include "base/logging.hh"
#include "cpu/simple-3-cycle-in-order-cpu/exec_context.hh"
#include "cpu/simple-3-cycle-in-order-cpu/lsq.hh"
#include "cpu/simple-3-cycle-in-order-cpu/simple_3cycle_cpu.hh"
#include "cpu/static_inst.hh"
#include "cpu/thread_context.hh"
#include "debug/Simple3CPUExecute.hh"
#include "sim/faults.hh"

namespace gem5
{
namespace simple3
{

Execute::Execute(Simple3CycleCPU &cpu_) : cpu(cpu_)
{
}

MispredictInfo
Execute::runOneCycle(ThreadContext *tc)
{
    LSQ &lsq = cpu.getLsq();

    // Stall: a previously-issued load/store hasn't responded yet.
    // The eSlot inst stays put — we'll re-enter when the response
    // arrives and the LSQ flips to Complete.
    if (lsq.pending()) {
        cpu.simple3Stats.memStallCycles++;
        DPRINTF(Simple3CPUExecute, "stall: LSQ pending\n");
        return MispredictInfo{false, 0};
    }

    if (!eSlot)
        return MispredictInfo{false, 0};

    DecodedSlotEntry e = *eSlot;

    SimpleThread &thread = *cpu.thread(0);
    panic_if(!e.pcStateAtDecode,
             "Execute: DecodedSlotEntry missing pcStateAtDecode");

    // Early-resolved branch: Pipeline's E→D forwarding step already
    // ran execute() at D-time and computed the next PC.  E becomes
    // a 1-cycle no-op pass-through (mirrors the no_art prototype's
    // "For BNE, E is a no-op because the branch was already resolved
    // in D" behaviour).  We still consume the eSlot and bump
    // instsCommitted so cycle-time and instruction counts both
    // include the bne — it just doesn't do any work here.
    if (e.earlyResolved) {
        eSlot.reset();
        cpu.simple3Stats.instsCommitted++;
        // Set the architectural PC to the resolved target now that
        // the inst has retired.  No execute() / advancePC needed.
        std::unique_ptr<PCStateBase> newPc(thread.pcState().clone());
        newPc->set(e.resolvedNpc);
        thread.pcState(*newPc);
        DPRINTF(Simple3CPUExecute,
                "commit (early-resolved) pc=%#x size=%u nextPc=%#x\n",
                e.addr, e.staticInst->size(), e.resolvedNpc);
        // No mispredict path — D-stage resolution is exact.
        return MispredictInfo{false, 0};
    }

    thread.pcState(*e.pcStateAtDecode);

    ExecContext ctx(cpu, thread);
    Fault fault = NoFault;

    if (lsq.complete()) {
        // Resuming a memory inst whose response just arrived.
        // completeAcc() reads the packet, writes any loaded value
        // back to the dest reg, and returns NoFault on success.
        PacketPtr pkt = lsq.completedPacket();
        DPRINTF(Simple3CPUExecute,
                "completeAcc pc=%#x cmd=%s\n",
                e.addr, pkt->cmdString());
        fault = e.staticInst->completeAcc(pkt, &ctx, /*traceData=*/nullptr);
        lsq.release();
    } else if (e.staticInst->isMemRef()) {
        // First touch of a memory inst: issue the request via the
        // LSQ and stall until the response arrives.  The actual
        // load/store happens in initiateAcc() through ExecContext's
        // initiateMemRead / writeMem hooks.
        DPRINTF(Simple3CPUExecute,
                "initiateAcc pc=%#x size=%u\n",
                e.addr, e.staticInst->size());
        fault = e.staticInst->initiateAcc(&ctx, /*traceData=*/nullptr);
        panic_if(fault != NoFault,
                 "Execute: fault from initiateAcc(): %s",
                 fault->name());
        if (lsq.pending()) {
            // Don't commit yet — wait for the response.  Leave eSlot
            // as-is so we re-enter this code path next cycle.
            DPRINTF(Simple3CPUExecute,
                    "stall: memref issued, awaiting response\n");
            return MispredictInfo{false, 0};
        }
        // initiateAcc didn't actually push anything (predicate
        // failed); fall through and treat like a non-mem inst.
    } else {
        // Non-memory inst: full execute in one cycle.
        fault = e.staticInst->execute(&ctx, /*traceData=*/nullptr);
        panic_if(fault != NoFault,
                 "Execute: fault from execute(): %s",
                 fault->name());
    }

    // We're committing this inst this cycle — clear eSlot now.
    eSlot.reset();
    cpu.simple3Stats.instsCommitted++;

    // advancePC reads pcState().npc and writes it back as the new pc.
    // For not-taken branches and non-branches, npc == e.addr + size.
    // For taken branches, execute() updated npc to the target.
    e.staticInst->advancePC(tc);
    const Addr actualNextPc = tc->pcState().instAddr();

    // With Pipeline's E→D forwarding (see earlyResolveBranchAtD)
    // every direct branch is pre-resolved and lands here with
    // earlyResolved=true, returning above.  Anything reaching this
    // line is either a non-branch, an indirect branch (BX/BLX reg)
    // we couldn't pre-execute, or a branch that fell through to its
    // sequential successor.  An indirect branch's actual target is
    // only known *now*, so if it differs from the sequential
    // fall-through we must squash F via the legacy mispredict path.
    const Addr fallThrough = e.addr + e.staticInst->size();
    const bool isIndirectMispred =
        e.staticInst->isControl() &&
        e.staticInst->isIndirectCtrl() &&
        actualNextPc != fallThrough;

    DPRINTF(Simple3CPUExecute,
            "commit pc=%#x size=%u nextPc=%#x indirectMispred=%d\n",
            e.addr, e.staticInst->size(),
            actualNextPc, (int)isIndirectMispred);

    if (isIndirectMispred)
        return MispredictInfo{true, actualNextPc};
    return MispredictInfo{false, 0};
}

} // namespace simple3
} // namespace gem5
