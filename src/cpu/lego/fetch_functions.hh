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

#ifndef __CPU_LEGO_FETCH_FUNCTIONS_HH__
#define __CPU_LEGO_FETCH_FUNCTIONS_HH__

#include "cpu/lego/port.hh"
#include "cpu/lego/stage_function.hh"
#include "cpu/lego/type.hh"
#include "params/FetchAddressGen.hh"
#include "params/FetchMemRequest.hh"

namespace gem5
{

/**
 * Reads PC from ThreadContext (or redirect input), aligns to fetch
 * width, sends MMU translation request. Writes FetchAddr output
 * when translation completes. Assigns seqNum to each new fetch.
 */
class FetchAddressGen : public StageFunctionTranslation
{
  public:
    PARAMS(FetchAddressGen);
    FetchAddressGen(const Params &params);

    void latchInputs() override;
    void compute() override;
    void flush() override;
    std::string traceStatus() const override;

  protected:
    void translationComplete(const Fault &fault,
        const RequestPtr &req, ThreadContext *tc,
        BaseMMU::Mode mode) override;

  private:
    Output<FetchAddr> fetchAddrOut;
    Input<Redirect> redirectIn;

    Redirect localRedirect;
    bool localRedirectValid = false;

    const size_t fetchWidth;
    InstSeqNum nextSeqNum;
    bool waitingForTranslation;
    Addr pendingPC;  // PC saved when translation was initiated
};

/**
 * Reads FetchAddr input, sends icache request, receives response
 * via recvTimingResp, outputs FetchLine.
 *
 * In a 2-stage fetch, this lives in stage 0. The FetchLine output
 * crosses to stage 1 (next cycle), naturally modeling cache latency.
 */
class FetchMemRequest : public StageFunctionWithPort
{
  public:
    PARAMS(FetchMemRequest);
    FetchMemRequest(const Params &params);

    void latchInputs() override;
    void compute() override;
    void flush() override;
    void recvTimingResp(PacketPtr pkt) override;
    std::string traceStatus() const override;

  private:
    Input<FetchAddr> fetchAddrIn;
    Output<FetchLine> fetchLineOut;

    FetchAddr localFetchAddr;
    bool localFetchAddrValid = false;

    bool waitingForCache;

    /** Last seqNum we sent a request for. */
    InstSeqNum lastRequestedSeqNum;
};

} // namespace gem5

#endif // __CPU_LEGO_FETCH_FUNCTIONS_HH__
