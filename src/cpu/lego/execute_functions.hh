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

#ifndef __CPU_LEGO_EXECUTE_FUNCTIONS_HH__
#define __CPU_LEGO_EXECUTE_FUNCTIONS_HH__

#include "cpu/lego/port.hh"
#include "cpu/lego/stage_function.hh"
#include "cpu/lego/type.hh"
#include "params/ALUExecute.hh"
#include "params/InstructionDecode.hh"
#include "params/PCUpdate.hh"

namespace gem5
{

/**
 * Reads FetchLine input, decodes using gem5's ISA decoder,
 * produces DecodedInst output.
 */
class InstructionDecode : public StageFunction
{
  public:
    PARAMS(InstructionDecode);
    InstructionDecode(const Params &params);

    void latchInputs() override;
    void compute() override;
    void flush() override;
    std::string traceStatus() const override;

  private:
    Input<FetchLine> fetchLineIn;
    Output<DecodedInst> decodedInstOut;

    FetchLine localFetchLine;
    bool localFetchLineValid = false;

    InstSeqNum lastDecodedSeqNum;
};

/**
 * Reads DecodedInst, executes using staticInst->execute(),
 * writes ExecResult output.
 */
class ALUExecute : public StageFunction
{
  public:
    PARAMS(ALUExecute);
    ALUExecute(const Params &params);


    void compute() override;
    void flush() override;
    std::string traceStatus() const override;

  private:
    Input<DecodedInst> decodedInstIn;
    Output<ExecResult> execResultOut;

    InstSeqNum lastExecutedSeqNum;
};

/**
 * Reads ExecResult, advances PC in ThreadContext.
 * If a branch was taken, writes Redirect output back to
 * FetchAddressGen (cross-stage, next cycle).
 */
class PCUpdate : public StageFunction
{
  public:
    PARAMS(PCUpdate);
    PCUpdate(const Params &params);


    void compute() override;
    void flush() override;
    std::string traceStatus() const override;

  private:
    Input<ExecResult> execResultIn;
    Output<Redirect> redirectOut;

    InstSeqNum lastUpdatedSeqNum;
};

} // namespace gem5

#endif // __CPU_LEGO_EXECUTE_FUNCTIONS_HH__
