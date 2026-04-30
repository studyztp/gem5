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

#ifndef __DEV_ARM_M_PROFILE_BRIDGE_IO_HH__
#define __DEV_ARM_M_PROFILE_BRIDGE_IO_HH__

/**
 * @file
 * Arm-M-profile bridge I/O device.
 *
 * Memory-mapped device that lets a gem5 Python config script:
 *   - Push input data into a buffer the simulated firmware can read.
 *   - Read output data the simulated firmware has written.
 *   - Raise / clear a configurable external IRQ on the M-profile NVIC
 *     (MProfileSCS).
 *   - Receive a "done" signal from firmware (firmware writes 1 to
 *     register[1], device calls exitSimLoopNow).
 *
 * Modeled after the generic BridgeIODevice from gem5 commit 0d8839ea,
 * but the interrupt path is replaced with a typed MProfileSCS pointer
 * and an IRQ index, since M-profile uses an NVIC instead of a GIC.
 */

#include <cstdint>
#include <vector>

#include "debug/MProfileBridgeIO.hh"
#include "dev/arm/m_profile_scs.hh"
#include "dev/io_device.hh"
#include "params/MProfileBridgeIO.hh"
#include "sim/sim_exit.hh"

namespace gem5
{

// Number of 32-bit control registers exposed at the start of the PIO
// range.  Layout (matches the reference BridgeIODevice exactly so that
// existing Python harnesses can be reused):
//   [0] go        — RO (latched at ctor from the 'go' param);
//                   non-zero means firmware should start consuming.
//   [1] done      — RW; firmware writing 1 here triggers exitSimLoopNow
//                   so a Python script can m5.simulate() until done.
//   [2] in_addr   — RO, absolute address of the input buffer.
//   [3] in_size   — RO, valid input bytes (set by Python via
//                   updateInputData()).
//   [4] out_addr  — RO, absolute address of the output buffer.
//   [5] out_size  — RW; firmware writes the produced output byte
//                   count after filling the output buffer (read back
//                   by Python via getOutputDataSize()).
constexpr unsigned MProfileBridgeIORegisterCount = 6;
using MProfileBridgeIORegSize = uint32_t;

class MProfileBridgeIO : public BasicPioDevice
{
  public:
    PARAMS(MProfileBridgeIO);
    MProfileBridgeIO(const Params &p);

    // Bounds-check irqNum against scs->params().num_irqs once both
    // SimObjects are fully constructed.  Cannot do this in the
    // constructor because the SCS Params struct may not be safely
    // accessible before init().
    void init() override;

    // -- PyBind exports (Python config drives these) --
    // Surface mirrors the reference BridgeIODevice so a Python
    // harness written for that device works here with a single
    // type-name change.
    bool updateDone(bool done);
    bool updateInputData(const std::vector<uint8_t> &data);
    bool raiseInterrupt();
    bool clearInterrupt();
    bool ifDone() const;
    std::vector<uint8_t> getOutputData() const;
    int getOutputDataSize() const;

  protected:
    Tick read(PacketPtr pkt) override;
    Tick write(PacketPtr pkt) override;

    // Strongly-typed pointer to the M-profile NVIC.  Replaces the
    // reference device's SimObject* + dynamic_cast<ArmSPI/ArmSPIGen>
    // path because M-profile uses MProfileSCS::sendInt/clearInt
    // instead of A-profile GIC SPI lines.  Set in the ctor from the
    // Param.MProfileSCS pointer.
    MProfileSCS *scs;

    // 0-based external IRQ index (the NVIC IRQn number that firmware
    // would use with NVIC->ISER[N/32], not the architectural exception
    // number).  Translated to exception number = irqNum + 16 inside
    // the SCS by sendInt/clearInt.
    uint32_t irqNum;

    MProfileBridgeIORegSize registers[MProfileBridgeIORegisterCount];
    std::vector<uint8_t> inputData;
    std::vector<uint8_t> outputData;
    size_t inputDataBufferSize;
    size_t outputDataBufferSize;

    // Absolute (system-bus) address ranges of the in/out buffers.
    // Cached at construction so read()/write() can dispatch with a
    // single pair of address comparisons per region.
    Addr inputDataStartAddr;
    Addr inputDataEndAddr;
    Addr outputDataStartAddr;
    Addr outputDataEndAddr;
};

} // namespace gem5

#endif // __DEV_ARM_M_PROFILE_BRIDGE_IO_HH__
