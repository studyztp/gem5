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

#include "dev/arm/m_profile_bridge_io.hh"

#include "base/logging.hh"
#include "base/trace.hh"
#include "mem/packet_access.hh"

namespace gem5
{

MProfileBridgeIO::MProfileBridgeIO(const Params &p)
    : BasicPioDevice(p, p.pio_size),
      scs(p.scs),
      irqNum(p.irq_num),
      inputData(p.input_data_buffer_size, 0),
      outputData(p.output_data_buffer_size, 0),
      inputDataBufferSize(p.input_data_buffer_size),
      outputDataBufferSize(p.output_data_buffer_size)
{
    // Layout sanity: 6 control registers + input buffer + output
    // buffer must fit inside the configured PIO window.  Use bytes
    // (sizeof(MProfileBridgeIORegSize) = 4) so a future change to
    // register width gets caught here.
    const size_t registerBytes =
        MProfileBridgeIORegisterCount * sizeof(MProfileBridgeIORegSize);
    panic_if(registerBytes + inputDataBufferSize + outputDataBufferSize
             > pioSize,
             "MProfileBridgeIO: pio_size=%llu too small for %u "
             "registers + %u input + %u output bytes",
             (unsigned long long)pioSize,
             (unsigned)registerBytes,
             (unsigned)inputDataBufferSize,
             (unsigned)outputDataBufferSize);

    // Register layout (see MProfileBridgeIORegisterCount comment in
    // the header for the per-register meaning).  pio_addr is the base
    // of the register block; the input buffer immediately follows the
    // registers, and the output buffer immediately follows the input
    // buffer.  Storing the addresses (rather than just offsets) lets
    // firmware read register[2] / register[4] and use the values
    // directly as pointers without doing arithmetic.
    registers[0] = p.go ? 1 : 0;
    registers[1] = 0;  // not yet 'done'

    registers[2] = p.pio_addr + registerBytes;
    inputDataStartAddr = registers[2];
    inputDataEndAddr   = inputDataStartAddr + inputDataBufferSize;

    registers[3] = 0;  // input data size — set by Python via
                       // updateInputData().

    registers[4] = inputDataEndAddr;
    outputDataStartAddr = registers[4];
    outputDataEndAddr   = outputDataStartAddr + outputDataBufferSize;

    registers[5] = 0;  // output data size — written by firmware after
                       // it produces results.

    DPRINTF(MProfileBridgeIO,
        "ctor: pio_addr=%#llx pio_size=%llu in=[%#llx,%#llx) "
        "out=[%#llx,%#llx) irq=%u\n",
        (unsigned long long)p.pio_addr,
        (unsigned long long)pioSize,
        (unsigned long long)inputDataStartAddr,
        (unsigned long long)inputDataEndAddr,
        (unsigned long long)outputDataStartAddr,
        (unsigned long long)outputDataEndAddr,
        irqNum);
}

void
MProfileBridgeIO::init()
{
    BasicPioDevice::init();

    // Bounds-check irqNum here (rather than in the ctor) so we read
    // scs->params() only after the SCS object's params struct is
    // fully populated.  fatal_if (not panic_if) so the user sees a
    // clean configuration error in their gem5 output rather than a
    // C++ stack trace.
    fatal_if(irqNum >= scs->params().num_irqs,
             "MProfileBridgeIO: irq_num=%u is out of range "
             "(SCS num_irqs=%u, valid range is 0..%u)",
             irqNum, (unsigned)scs->params().num_irqs,
             (unsigned)scs->params().num_irqs - 1);
}

Tick
MProfileBridgeIO::read(PacketPtr pkt)
{
    Addr addr = pkt->getAddr();
    const Addr regsBytes =
        MProfileBridgeIORegisterCount * sizeof(MProfileBridgeIORegSize);

    // -- Register region [pio_addr, pio_addr + regsBytes) --
    if (addr >= pioAddr && addr < pioAddr + regsBytes) {
        // Force word-aligned 4-byte register access.  Allowing
        // sub-word reads here would force us to model byte-lane
        // semantics on RAZ/RAO bits, which the reference device
        // doesn't do either.
        panic_if(pkt->getSize() != sizeof(MProfileBridgeIORegSize),
                 "MProfileBridgeIO: register reads must be 4 bytes "
                 "(got %u)", (unsigned)pkt->getSize());
        Addr offset = addr - pioAddr;
        size_t idx = offset / sizeof(MProfileBridgeIORegSize);
        DPRINTF(MProfileBridgeIO,
            "read reg[%u] addr=%#llx -> %#x\n",
            (unsigned)idx, (unsigned long long)addr, registers[idx]);
        pkt->setData(reinterpret_cast<uint8_t *>(&registers[idx]));
        pkt->makeResponse();
        return pioDelay;
    }

    // -- Input data buffer (firmware reads, Python writes) --
    if (addr >= inputDataStartAddr && addr < inputDataEndAddr) {
        size_t size = pkt->getSize();
        Addr offset = addr - inputDataStartAddr;
        panic_if(offset + size > inputDataBufferSize,
                 "MProfileBridgeIO: input read offset=%#llx size=%u "
                 "overflows buffer (size=%u)",
                 (unsigned long long)offset, (unsigned)size,
                 (unsigned)inputDataBufferSize);
        DPRINTF(MProfileBridgeIO,
            "read input[%#llx..+%u]\n",
            (unsigned long long)offset, (unsigned)size);
        pkt->setData(&inputData[offset]);
        pkt->makeResponse();
        return pioDelay;
    }

    panic("MProfileBridgeIO::read: addr %#llx out of range "
          "(pio=[%#llx,%#llx) in=[%#llx,%#llx) out=[%#llx,%#llx))",
          (unsigned long long)addr,
          (unsigned long long)pioAddr,
          (unsigned long long)(pioAddr + regsBytes),
          (unsigned long long)inputDataStartAddr,
          (unsigned long long)inputDataEndAddr,
          (unsigned long long)outputDataStartAddr,
          (unsigned long long)outputDataEndAddr);
}

Tick
MProfileBridgeIO::write(PacketPtr pkt)
{
    Addr addr = pkt->getAddr();
    const Addr regsBytes =
        MProfileBridgeIORegisterCount * sizeof(MProfileBridgeIORegSize);

    // -- Register region --
    if (addr >= pioAddr && addr < pioAddr + regsBytes) {
        panic_if(pkt->getSize() != sizeof(MProfileBridgeIORegSize),
                 "MProfileBridgeIO: register writes must be 4 bytes "
                 "(got %u)", (unsigned)pkt->getSize());
        Addr offset = addr - pioAddr;
        size_t idx = offset / sizeof(MProfileBridgeIORegSize);
        pkt->writeData(reinterpret_cast<uint8_t *>(&registers[idx]));
        DPRINTF(MProfileBridgeIO,
            "write reg[%u] addr=%#llx <- %#x\n",
            (unsigned)idx, (unsigned long long)addr, registers[idx]);
        pkt->makeResponse();

        // 'done' channel: firmware writing 1 to register[1] tells
        // gem5 the workload has finished.  Calling exitSimLoopNow
        // here is safe — it schedules an exit event at the current
        // tick rather than unwinding the call stack, so the packet
        // response we just constructed is delivered cleanly first.
        if (idx == 1 && registers[1] == 1) {
            exitSimLoopNow("MProfileBridgeIO signaled done.");
        }
        return pioDelay;
    }

    // -- Output data buffer (firmware writes, Python reads) --
    if (addr >= outputDataStartAddr && addr < outputDataEndAddr) {
        size_t size = pkt->getSize();
        Addr offset = addr - outputDataStartAddr;
        panic_if(offset + size > outputDataBufferSize,
                 "MProfileBridgeIO: output write offset=%#llx size=%u "
                 "overflows buffer (size=%u)",
                 (unsigned long long)offset, (unsigned)size,
                 (unsigned)outputDataBufferSize);
        pkt->writeData(&outputData[offset]);
        DPRINTF(MProfileBridgeIO,
            "write output[%#llx..+%u]\n",
            (unsigned long long)offset, (unsigned)size);
        pkt->makeResponse();
        return pioDelay;
    }

    panic("MProfileBridgeIO::write: addr %#llx out of range "
          "(pio=[%#llx,%#llx) in=[%#llx,%#llx) out=[%#llx,%#llx))",
          (unsigned long long)addr,
          (unsigned long long)pioAddr,
          (unsigned long long)(pioAddr + regsBytes),
          (unsigned long long)inputDataStartAddr,
          (unsigned long long)inputDataEndAddr,
          (unsigned long long)outputDataStartAddr,
          (unsigned long long)outputDataEndAddr);
}

bool
MProfileBridgeIO::updateDone(bool done)
{
    // Allows a Python script to flip the 'done' bit programmatically
    // (e.g., to reset for another iteration).  Distinct from the
    // firmware-driven 'done' write path in write(), which exits the
    // sim loop — calls here intentionally do NOT exit, so Python can
    // clear the flag without ending the simulation.
    registers[1] = done ? 1 : 0;
    DPRINTF(MProfileBridgeIO, "updateDone -> %u\n", registers[1]);
    return true;
}

bool
MProfileBridgeIO::updateInputData(const std::vector<uint8_t> &data)
{
    // Refuse to populate input while the device is 'off' so a Python
    // harness can use registers[0]=0 as a hold-off signal during
    // setup.  Also refuse oversized writes — firmware only knows the
    // buffer size at compile time, so silently truncating would be a
    // worse failure mode than a refusal the harness can detect.
    if (!registers[0]) {
        DPRINTF(MProfileBridgeIO,
            "updateInputData rejected: device not on (go=0)\n");
        return false;
    }
    if (data.size() > inputDataBufferSize) {
        DPRINTF(MProfileBridgeIO,
            "updateInputData rejected: %u > buffer size %u\n",
            (unsigned)data.size(), (unsigned)inputDataBufferSize);
        return false;
    }
    inputData.assign(data.begin(), data.end());
    // Pad the rest of the buffer with zeroes so stale data from a
    // previous invocation cannot be read by firmware that ignores
    // registers[3] (the size register).
    if (data.size() < inputDataBufferSize) {
        std::fill(inputData.begin() + data.size(),
                  inputData.end(), 0);
    }
    registers[3] = data.size();
    DPRINTF(MProfileBridgeIO,
        "updateInputData: %u bytes\n", (unsigned)data.size());
    return true;
}

bool
MProfileBridgeIO::raiseInterrupt()
{
    // Honour the 'go' gate so a Python harness can keep the device
    // quiet during initial setup (matches updateInputData() policy).
    if (!registers[0]) {
        DPRINTF(MProfileBridgeIO,
            "raiseInterrupt rejected: device not on (go=0)\n");
        return false;
    }
    DPRINTF(MProfileBridgeIO, "raiseInterrupt -> SCS irq=%u\n", irqNum);
    scs->sendInt(irqNum);
    return true;
}

bool
MProfileBridgeIO::clearInterrupt()
{
    // Clearing is allowed regardless of 'go' — a Python harness may
    // need to clean up a previously-raised IRQ during teardown after
    // turning the device off.
    DPRINTF(MProfileBridgeIO, "clearInterrupt -> SCS irq=%u\n", irqNum);
    scs->clearInt(irqNum);
    return true;
}

bool
MProfileBridgeIO::ifDone() const
{
    return registers[1] == 1;
}

std::vector<uint8_t>
MProfileBridgeIO::getOutputData() const
{
    // Returns by value — Python side gets a copy, simulator state
    // is not aliased.  Caller decides how many bytes are valid via
    // getOutputDataSize() (= registers[5]).
    return outputData;
}

int
MProfileBridgeIO::getOutputDataSize() const
{
    return static_cast<int>(registers[5]);
}

} // namespace gem5
