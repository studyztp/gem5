/*
 * Copyright (c) 2026 University of California, Davis and Cornell University
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

#ifndef __MEM_PIPELINED_SIMPLE_MEMORY_HH__
#define __MEM_PIPELINED_SIMPLE_MEMORY_HH__

#include "mem/simple_mem.hh"
#include "params/PipelinedSimpleMemory.hh"

namespace gem5
{

namespace memory
{

/**
 * A pipelined memory model inheriting from SimpleMemory.
 *
 * Models two AHB-Lite behaviors:
 *  1. Address/data phase overlap: accepts up to max_outstanding
 *     concurrent requests instead of blocking on isBusy.
 *  2. Read buffer: serves subsequent reads from the same aligned block
 *     at buffer_hit_cycles instead of the full latency, modelling a
 *     flash controller's internal read register.
 */
class PipelinedSimpleMemory : public SimpleMemory
{
  private:
    /** Maximum number of outstanding requests. */
    const unsigned maxOutstanding;

    /** Minimum cycles between accepting consecutive requests. */
    const Cycles addressPhaseCycles;

    /** Tick at which the next request can be accepted. */
    Tick nextAcceptTick;

    /** Read buffer size in bytes (0 = disabled, must be power of 2). */
    const unsigned readBufferSize;

    /** Latency for a read buffer hit. */
    const Cycles bufferHitCycles;

    /** Address of the currently buffered aligned block (~0 = invalid). */
    Addr bufferedBlockAddr;

    /** Tick at which the last response will be sent.  Used to enforce
     *  in-order response delivery: each new response's data phase cannot
     *  start until the previous one completes (AHB ordered bus). */
    Tick lastResponseTick;

    /** Event to send retry when a queue slot opens. */
    EventFunctionWrapper retryEvent;

    /** Send retry to upstream if appropriate. */
    void sendRetry();

  protected:
    bool recvTimingReq(PacketPtr pkt) override;

  public:
    PipelinedSimpleMemory(const PipelinedSimpleMemoryParams &p);
};

} // namespace memory
} // namespace gem5

#endif // __MEM_PIPELINED_SIMPLE_MEMORY_HH__
