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

#include "cpu/signal-3-stage-in-order-cpu/alu_fu.hh"

#include <sstream>

#include "base/logging.hh"
#include "debug/Signal3CPUExecute.hh"

namespace gem5
{
namespace signal3
{

AluFunctionUnit::AluFunctionUnit(SignalCPU &cpu)
    : _cpu(cpu)
{
}

bool
AluFunctionUnit::accepts(const StaticInstPtr &inst)
{
    if (!inst)
        return false;
    // Memory ops go through the LSQ; control ops (branches) are
    // resolved directly in E because the redirect path is
    // structural (e_redirect_to_f / d_redirect_to_f).  Floating-
    // point ops aren't ALU and aren't modeled here yet.
    if (inst->isMemRef())
        return false;
    if (inst->isControl())
        return false;
    if (inst->isFloating())
        return false;
    // The remainder is the integer-ALU path (nop, mov, add, sub,
    // cmp, and, orr, lsl, lsr, asr, mul, ...).  Most are 1-cy on
    // Cortex-M4; UDIV/SDIV are 2-12 cy per TRM Table 3-1, which
    // future iterations of `setLatency()` will tag per-opcode.
    return inst->isInteger();
}

void
AluFunctionUnit::issue(const StaticInstPtr &inst)
{
    panic_if(_state != State::Idle,
             "AluFunctionUnit::issue called while not Idle (state=%d)",
             (int)_state);
    panic_if(!accepts(inst),
             "AluFunctionUnit::issue: inst doesn't pass accepts()");

    _inFlight = inst;

    if (_latency <= 1) {
        // 1-cy fast path: complete same cycle as issue.  E sees
        // complete() return true immediately and commits without
        // any cycle delay (preserves existing nop/mov/add timing).
        _state = State::Complete;
        _cyclesLeft = 0;
        DPRINTF(Signal3CPUExecute,
                "ALU.issue inst=%s -> Complete (1-cy fast path)\n",
                inst->getName().c_str());
    } else {
        _state = State::Pending;
        _cyclesLeft = _latency - 1;
        DPRINTF(Signal3CPUExecute,
                "ALU.issue inst=%s -> Pending (%u cy remaining)\n",
                inst->getName().c_str(), _cyclesLeft);
    }
}

void
AluFunctionUnit::tick()
{
    // Advance Pending toward Complete.  Called once per CPU cycle
    // by Pipeline::evaluate before Settle so that E observes the
    // post-tick state.
    if (_state != State::Pending)
        return;

    panic_if(_cyclesLeft == 0,
             "AluFunctionUnit::tick: Pending with 0 cycles left");
    --_cyclesLeft;
    if (_cyclesLeft == 0) {
        _state = State::Complete;
        DPRINTF(Signal3CPUExecute,
                "ALU.tick -> Complete\n");
    }
}

void
AluFunctionUnit::release()
{
    if (_state == State::Idle)
        return;
    DPRINTF(Signal3CPUExecute, "ALU.release\n");
    _state = State::Idle;
    _cyclesLeft = 0;
    _inFlight = nullptr;
}

void
AluFunctionUnit::reset()
{
    _state = State::Idle;
    _cyclesLeft = 0;
    _inFlight = nullptr;
}

std::string
AluFunctionUnit::snapshotString() const
{
    std::ostringstream os;
    os << "alu=";
    switch (_state) {
      case State::Idle:     os << "Idle"; break;
      case State::Pending:  os << "Pending(" << _cyclesLeft << ")"; break;
      case State::Complete: os << "Complete"; break;
    }
    return os.str();
}

} // namespace signal3
} // namespace gem5
