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

#ifndef __CPU_LEGO_EXEC_CONTEXT_HH__
#define __CPU_LEGO_EXEC_CONTEXT_HH__

#include "cpu/base.hh"
#include "cpu/exec_context.hh"
#include "cpu/simple_thread.hh"

namespace gem5
{

/**
 * Minimal ExecContext for the Lego CPU.
 * Delegates register/PC access to SimpleThread.
 * Memory operations are not supported (base case: ALU only).
 */
class LegoExecContext : public ExecContext
{
  public:
    BaseCPU &cpu;
    SimpleThread &thread;

    LegoExecContext(BaseCPU &cpu_, SimpleThread &thread_)
        : cpu(cpu_), thread(thread_) {}

    // --- Register access ---
    RegVal
    getRegOperand(const StaticInst *si, int idx) override
    {
        const RegId &reg = si->srcRegIdx(idx);
        return thread.getReg(reg);
    }

    void
    getRegOperand(const StaticInst *si, int idx, void *val) override
    {
        const RegId &reg = si->srcRegIdx(idx);
        thread.getReg(reg, val);
    }

    void *
    getWritableRegOperand(const StaticInst *si, int idx) override
    {
        const RegId &reg = si->destRegIdx(idx);
        return thread.getWritableReg(reg);
    }

    void
    setRegOperand(const StaticInst *si, int idx, RegVal val) override
    {
        const RegId &reg = si->destRegIdx(idx);
        thread.setReg(reg, val);
    }

    void
    setRegOperand(const StaticInst *si, int idx,
                  const void *val) override
    {
        const RegId &reg = si->destRegIdx(idx);
        thread.setReg(reg, val);
    }

    // --- Misc register access ---
    RegVal
    readMiscRegOperand(const StaticInst *si, int idx) override
    {
        const RegId &reg = si->srcRegIdx(idx);
        return thread.readMiscReg(reg.index());
    }

    void
    setMiscRegOperand(const StaticInst *si, int idx,
                      RegVal val) override
    {
        const RegId &reg = si->destRegIdx(idx);
        thread.setMiscReg(reg.index(), val);
    }

    RegVal
    readMiscReg(int misc_reg) override
    { return thread.readMiscReg(misc_reg); }

    void
    setMiscReg(int misc_reg, RegVal val) override
    { thread.setMiscReg(misc_reg, val); }

    // --- PC state ---
    const PCStateBase &
    pcState() const override
    { return thread.pcState(); }

    void
    pcState(const PCStateBase &val) override
    { thread.pcState(val); }

    // --- Memory (not supported in base case) ---
    Fault
    readMem(Addr addr, uint8_t *data, unsigned int size,
            Request::Flags flags,
            const std::vector<bool> &byte_enable) override
    { panic("LegoExecContext: readMem not implemented\n"); }

    Fault
    initiateMemRead(Addr addr, unsigned int size,
                    Request::Flags flags,
                    const std::vector<bool> &byte_enable) override
    { panic("LegoExecContext: initiateMemRead not implemented\n"); }

    Fault
    writeMem(uint8_t *data, unsigned int size, Addr addr,
             Request::Flags flags, uint64_t *res,
             const std::vector<bool> &byte_enable) override
    { panic("LegoExecContext: writeMem not implemented\n"); }

    Fault
    initiateMemMgmtCmd(Request::Flags flags) override
    { panic("LegoExecContext: initiateMemMgmtCmd not implemented\n"); }

    Fault
    amoMem(Addr addr, uint8_t *data, unsigned int size,
           Request::Flags flags,
           AtomicOpFunctorPtr amo_op) override
    { panic("LegoExecContext: amoMem not implemented\n"); }

    Fault
    initiateMemAMO(Addr addr, unsigned int size,
                   Request::Flags flags,
                   AtomicOpFunctorPtr amo_op) override
    { panic("LegoExecContext: initiateMemAMO not implemented\n"); }

    // --- Predicate ---
    bool readPredicate() const override { return thread.readPredicate(); }
    void setPredicate(bool val) override { thread.setPredicate(val); }
    bool
    readMemAccPredicate() const override
    { return thread.readMemAccPredicate(); }
    void
    setMemAccPredicate(bool val) override
    { thread.setMemAccPredicate(val); }

    // --- Store conditional ---
    unsigned int
    readStCondFailures() const override
    { return thread.readStCondFailures(); }
    void
    setStCondFailures(unsigned int sc_failures) override
    { thread.setStCondFailures(sc_failures); }

    // --- ThreadContext ---
    ThreadContext *tcBase() const override { return thread.getTC(); }

    // --- HTM (not supported) ---
    uint64_t newHtmTransactionUid() const override { return 0; }
    uint64_t getHtmTransactionUid() const override { return 0; }
    bool inHtmTransactionalState() const override { return false; }
    uint64_t getHtmTransactionalDepth() const override { return 0; }

    // --- Monitor/mwait ---
    void demapPage(Addr vaddr, uint64_t asn) override
    { thread.demapPage(vaddr, asn); }

    void
    armMonitor(Addr address) override
    { cpu.armMonitor(thread.threadId(), address); }

    bool
    mwait(PacketPtr pkt) override
    { return cpu.mwait(thread.threadId(), pkt); }

    void
    mwaitAtomic(ThreadContext *tc) override
    { cpu.mwaitAtomic(thread.threadId(), tc, thread.mmu); }

    AddressMonitor *
    getAddrMonitor() override
    { return cpu.getCpuAddrMonitor(thread.threadId()); }
};

} // namespace gem5

#endif // __CPU_LEGO_EXEC_CONTEXT_HH__
