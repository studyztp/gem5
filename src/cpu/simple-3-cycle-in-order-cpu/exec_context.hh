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

#ifndef __CPU_SIMPLE_3_CYCLE_IN_ORDER_CPU_EXEC_CONTEXT_HH__
#define __CPU_SIMPLE_3_CYCLE_IN_ORDER_CPU_EXEC_CONTEXT_HH__

#include "base/logging.hh"
#include "cpu/exec_context.hh"
#include "cpu/reg_class.hh"
#include "cpu/simple-3-cycle-in-order-cpu/lsq.hh"
#include "cpu/simple-3-cycle-in-order-cpu/simple_3cycle_cpu.hh"
#include "cpu/simple_thread.hh"
#include "cpu/static_inst.hh"
#include "mem/request.hh"

namespace gem5
{
namespace simple3
{

class Simple3CycleCPU;

/**
 * Minimal ExecContext for Simple3CycleCPU.
 *
 * Wraps a SimpleThread for register / PC / misc-reg / predicate
 * access.  Memory operations and HTM/sysop interfaces panic — the
 * Phase D.1 cut targets ALU + branch only.  Loads/stores get wired
 * up in Phase D.2 against the dcache port.
 */
class ExecContext : public gem5::ExecContext
{
  public:
    Simple3CycleCPU &cpu;
    SimpleThread &thread;

    ExecContext(Simple3CycleCPU &cpu_, SimpleThread &thread_)
        : cpu(cpu_), thread(thread_)
    {}

    // ---------- registers ----------------------------------------------

    RegVal
    getRegOperand(const StaticInst *si, int idx) override
    {
        const RegId &reg = si->srcRegIdx(idx);
        if (reg.is(InvalidRegClass))
            return 0;
        return thread.getReg(reg);
    }

    void
    getRegOperand(const StaticInst *si, int idx, void *val) override
    {
        thread.getReg(si->srcRegIdx(idx), val);
    }

    void *
    getWritableRegOperand(const StaticInst *si, int idx) override
    {
        return thread.getWritableReg(si->destRegIdx(idx));
    }

    void
    setRegOperand(const StaticInst *si, int idx, RegVal val) override
    {
        const RegId &reg = si->destRegIdx(idx);
        if (reg.is(InvalidRegClass))
            return;
        thread.setReg(reg, val);
    }

    void
    setRegOperand(const StaticInst *si, int idx, const void *val) override
    {
        thread.setReg(si->destRegIdx(idx), val);
    }

    // ---------- misc registers -----------------------------------------

    RegVal
    readMiscRegOperand(const StaticInst *si, int idx) override
    {
        const RegId &reg = si->srcRegIdx(idx);
        assert(reg.is(MiscRegClass));
        return thread.readMiscReg(reg.index());
    }

    void
    setMiscRegOperand(const StaticInst *si, int idx, RegVal val) override
    {
        const RegId &reg = si->destRegIdx(idx);
        assert(reg.is(MiscRegClass));
        thread.setMiscReg(reg.index(), val);
    }

    RegVal readMiscReg(int misc_reg) override
    {
        return thread.readMiscReg(misc_reg);
    }

    void setMiscReg(int misc_reg, RegVal val) override
    {
        thread.setMiscReg(misc_reg, val);
    }

    // ---------- PC -----------------------------------------------------

    const PCStateBase &pcState() const override { return thread.pcState(); }
    void pcState(const PCStateBase &val) override { thread.pcState(val); }

    // ---------- predicates ---------------------------------------------

    bool readPredicate() const override { return thread.readPredicate(); }
    void setPredicate(bool val) override { thread.setPredicate(val); }
    bool readMemAccPredicate() const override
    { return thread.readMemAccPredicate(); }
    void setMemAccPredicate(bool val) override
    { thread.setMemAccPredicate(val); }

    // ---------- thread / misc ------------------------------------------

    ThreadContext *tcBase() const override { return thread.getTC(); }

    unsigned int readStCondFailures() const override { return 0; }
    void setStCondFailures(unsigned int) override {}

    // ---------- memory ------------------------------------------------
    //
    // M-profile addresses are physical (identity translation), so we
    // bypass the MMU and push straight into the LSQ.

    Fault
    initiateMemRead(Addr addr, unsigned int size, Request::Flags flags,
                    const std::vector<bool> &/*byte_enable*/) override
    {
        return cpu.getLsq().pushReadRequest(addr, size, flags);
    }

    Fault
    writeMem(uint8_t *data, unsigned int size, Addr addr,
             Request::Flags flags, uint64_t */*res*/,
             const std::vector<bool> &/*byte_enable*/) override
    {
        return cpu.getLsq().pushWriteRequest(data, addr, size, flags);
    }

    Fault
    initiateMemMgmtCmd(Request::Flags) override
    {
        panic("Simple3CycleCPU ExecContext: mem-mgmt cmds not "
              "implemented");
    }

    Fault
    initiateMemAMO(Addr, unsigned int, Request::Flags,
                   AtomicOpFunctorPtr) override
    {
        panic("Simple3CycleCPU ExecContext: AMO not implemented");
    }

    // ---------- HTM (unused on M-profile) ------------------------------

    uint64_t getHtmTransactionUid() const override
    { panic("HTM not supported"); }
    uint64_t newHtmTransactionUid() const override
    { panic("HTM not supported"); }
    bool inHtmTransactionalState() const override { return false; }
    uint64_t getHtmTransactionalDepth() const override { return 0; }

    // ---------- monitor/mwait/demap (unused) ---------------------------

    void demapPage(Addr, uint64_t) override
    { panic("demapPage not supported"); }
    void armMonitor(Addr) override { panic("armMonitor not supported"); }
    bool mwait(PacketPtr) override { panic("mwait not supported"); }
    void mwaitAtomic(ThreadContext *) override
    { panic("mwaitAtomic not supported"); }
    AddressMonitor *getAddrMonitor() override
    { panic("getAddrMonitor not supported"); }
};

} // namespace simple3
} // namespace gem5

#endif // __CPU_SIMPLE_3_CYCLE_IN_ORDER_CPU_EXEC_CONTEXT_HH__
