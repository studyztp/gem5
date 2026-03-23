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

#ifndef __ARCH_ARM_M_MMU_HH__
#define __ARCH_ARM_M_MMU_HH__

/**
 * @file
 * Pass-through MMU and TLB for M-profile (no address translation).
 *
 * M-profile has no MMU — all addresses are physical.  These classes
 * satisfy the BaseMMU/BaseTLB interface that gem5 CPU models require
 * by performing identity translation (VA == PA, always NoFault).
 *
 * TODO: When the optional MPU (Memory Protection Unit) is modelled,
 * permission checks can be added to MTLB::translateAtomic() etc.
 */

#include <deque>
#include <vector>

#include "arch/generic/mmu.hh"
#include "arch/generic/tlb.hh"
#include "mem/packet_queue.hh"
#include "mem/port.hh"
#include "mem/qport.hh"
#include "params/ArmMMMU.hh"
#include "params/ArmMTLB.hh"
#include "params/StackingBarrier.hh"
#include "sim/clocked_object.hh"

namespace gem5
{

namespace ArmISA
{

/**
 * Pass-through TLB — always returns NoFault, sets paddr = vaddr.
 */
class MTLB : public BaseTLB
{
  public:
    PARAMS(ArmMTLB);
    MTLB(const Params &p) : BaseTLB(p) {}

    void demapPage(Addr vaddr, uint64_t asn) override {}

    Fault translateAtomic(const RequestPtr &req, ThreadContext *tc,
                          BaseMMU::Mode mode) override;

    void translateTiming(const RequestPtr &req, ThreadContext *tc,
                         BaseMMU::Translation *translation,
                         BaseMMU::Mode mode) override;

    Fault translateFunctional(const RequestPtr &req, ThreadContext *tc,
                              BaseMMU::Mode mode) override;

    Fault finalizePhysical(const RequestPtr &req, ThreadContext *tc,
                           BaseMMU::Mode mode) const override;

    void flushAll() override {}
    void takeOverFrom(BaseTLB *otlb) override {}
};

/**
 * Stacking barrier — sits between the CPU icache port and memory.
 *
 * On Cortex-M, exception entry pushes an 8-word stack frame to SRAM
 * via a dedicated hardware sequencer on the data bus.  The CPU
 * pipeline is stalled until stacking completes, but the instruction
 * bus can fetch the exception handler vector in parallel (Harvard
 * architecture with separate ICode and DCode/System buses).
 *
 * This barrier models that behavior without modifying any CPU model:
 *   - Normally transparent: requests and responses pass through.
 *   - When MMMU calls startHolding() (stacking begins), the barrier
 *     continues forwarding fetch requests to the cache/memory (so
 *     the icache can fill from Flash/SRAM), but queues responses
 *     instead of forwarding them to the CPU.
 *   - When MMMU calls releaseAll() (stacking completes), all held
 *     responses are forwarded to the CPU, which then proceeds to
 *     execute the handler.
 *
 * The CPU naturally stalls waiting for the icache response (this
 * is normal behavior for all CPU models).  No CPU model changes
 * are needed.
 *
 * Standalone ClockedObject with two custom port classes:
 *   - CpuSidePort (ResponsePort): faces the CPU icache port
 *   - MemSidePort (RequestPort): faces the cache or memory bus
 *
 * References:
 *   - Cortex-M3 TRM DDI0337G Section 5.5.1: exception entry timing
 *   - Cortex-M4 TRM DDI0439D Section 3.9: interrupt latency
 *   - DDI0403E B1.5.6: exception entry sequence
 */
class StackingBarrier : public ClockedObject
{
  private:
    class CpuSidePort : public ResponsePort
    {
      private:
        StackingBarrier &barrier;

      public:
        CpuSidePort(const std::string &name, StackingBarrier &barrier);

      protected:
        // Forward fetch requests from CPU to the memory side.
        bool recvTimingReq(PacketPtr pkt) override;

        // Forward retry from CPU to memory side.
        void recvRespRetry() override;

        // Atomic path (AtomicSimpleCPU): pass through directly.
        Tick recvAtomic(PacketPtr pkt) override;

        // Functional path: pass through directly.
        void recvFunctional(PacketPtr pkt) override;

        // Address ranges: forward from memory side.
        AddrRangeList getAddrRanges() const override;
    };

    class MemSidePort : public RequestPort
    {
      private:
        StackingBarrier &barrier;

      public:
        MemSidePort(const std::string &name, StackingBarrier &barrier);

      protected:
        // Receive responses from cache/memory.
        // If holding, queue the response; otherwise forward to CPU.
        bool recvTimingResp(PacketPtr pkt) override;

        // Forward retry from memory side to CPU side.
        void recvReqRetry() override;

        // Forward range changes from memory side to CPU side.
        void recvRangeChange() override;
    };

    CpuSidePort cpuSidePort;
    MemSidePort memSidePort;

    /**
     * When true, icache responses are queued instead of forwarded
     * to the CPU.  Set by startHolding(), cleared by releaseAll().
     */
    bool holdResponses = false;

    /**
     * Responses held while stacking is in progress.
     * Released in FIFO order when releaseAll() is called.
     */
    std::deque<PacketPtr> heldResponses;

    /**
     * True if the CPU side needs a retry notification after we
     * were unable to forward a response (CPU side was busy).
     */
    bool retryResp = false;

    /**
     * True if the memory side needs a retry notification after
     * a request send failed.
     */
    bool retryReq = false;

  public:
    PARAMS(StackingBarrier);
    StackingBarrier(const Params &p);

    Port &getPort(const std::string &if_name,
                  PortID idx = InvalidPortID) override;

    /**
     * Begin holding icache responses.
     * Called by MMMU when exception frame stacking starts.
     * Fetch requests still pass through so the icache can begin
     * filling from Flash/SRAM in parallel with stacking writes.
     */
    void startHolding();

    /**
     * Release all held responses and resume normal operation.
     * Called by MMMU when exception frame stacking completes.
     * Held responses are forwarded to the CPU in FIFO order.
     */
    void releaseAll();

    /** True if the barrier is currently holding responses. */
    bool isHolding() const { return holdResponses; }
};

/**
 * Pass-through MMU for M-profile — identity translation + stacking engine.
 *
 * Delegates to MTLB for VA==PA translation.  Also owns the exception
 * frame stacking/unstacking engine: a dedicated StackingPort sends
 * timed store (push) and load (pop) requests to SRAM through the
 * memory bus, separate from the CPU's dcache port.  This models the
 * real Cortex-M hardware's dedicated data bus path for the stacking
 * sequencer (Cortex-M3 TRM DDI0337G Section 5.5.1).
 *
 * MMMU also controls the StackingBarrier (child SimObject) to hold
 * icache responses until stacking completes, modeling the CPU stall
 * during exception entry.
 */
class MMMU : public BaseMMU
{
  public:
    /**
     * Port for exception frame stacking writes and unstacking reads.
     *
     * Inherits from QueuedRequestPort which provides automatic
     * request queuing and retry handling via ReqPacketQueue.
     * Requests are scheduled with 1-cycle spacing via
     * schedTimingReq() — the queue handles bus backpressure
     * and retries automatically.
     *
     * A SnoopRespPacketQueue is required by QueuedRequestPort but
     * unused (M-profile has no cache snooping).
     *
     * Only recvTimingResp() needs to be implemented to count
     * responses and signal stacking completion.
     */
    class StackingPort : public QueuedRequestPort
    {
      private:
        MMMU &mmu;
        ReqPacketQueue reqPktQueue;
        SnoopRespPacketQueue snoopRespQueue;

      public:
        StackingPort(const std::string &name, MMMU &mmu);

      protected:
        /**
         * Receive response from memory for a stacking write or
         * unstacking read.  Decrements the pending response counter
         * and signals completion when all responses have arrived.
         */
        bool recvTimingResp(PacketPtr pkt) override;
    };

  private:
    StackingPort stackingPort;

    /** Pointer to the stacking barrier (child SimObject). */
    StackingBarrier *barrier = nullptr;

    // -- Stacking engine state --

    /** Requestor ID for stacking port memory requests.
     *  Registered during init() — gem5 requires requestor IDs
     *  to be registered before regStats(). */
    RequestorID stackingRequestorId = Request::invldRequestorId;

    /** Number of timing responses still outstanding.
     *  When this reaches 0, the barrier is released. */
    uint32_t pendingResponses = 0;

    /** Called when pendingResponses reaches 0.
     *  Releases the stacking barrier. */
    void stackingComplete();

  public:
    PARAMS(ArmMMMU);
    MMMU(const Params &p);

    Port &getPort(const std::string &if_name,
                  PortID idx = InvalidPortID) override;

    TranslationGenPtr translateFunctional(
            Addr start, Addr size, ThreadContext *tc,
            Mode mode, Request::Flags flags) override;

    /** Get the stacking barrier for external control. */
    StackingBarrier *getStackingBarrier() { return barrier; }

    /**
     * Push exception frame to stack via timed memory writes.
     *
     * Schedules one 32-bit write per cycle through the stackingPort,
     * modeling the AHB pipelining of the Cortex-M stacking
     * sequencer.  The stacking barrier is activated to hold icache
     * responses until all writes complete.
     *
     * @param frameptr  Stack address (decremented SP) for word 0.
     * @param values    Words to push.  Size determines number of
     *                  writes (8 for basic, 26 for extended FPU).
     * @param tc        ThreadContext for clock period and requestor ID.
     */
    void storeToStack(Addr frameptr, const std::vector<uint32_t> &values,
                      ThreadContext *tc);

    /**
     * Pop exception frame from stack.
     *
     * Reads numWords via sendFunctional() for immediate data, then
     * schedules timing read requests (1 per cycle) for realistic
     * latency.  The barrier holds the CPU until all timing reads
     * complete.
     *
     * @param frameptr  Stack address for word 0.
     * @param numWords  Number of 32-bit words to read.
     * @param tc        ThreadContext for clock period and requestor ID.
     * @return Vector of read values in address order.
     */
    std::vector<uint32_t> readFromStack(Addr frameptr, uint32_t numWords,
                                        ThreadContext *tc);
};

} // namespace ArmISA
} // namespace gem5

#endif // __ARCH_ARM_M_MMU_HH__
