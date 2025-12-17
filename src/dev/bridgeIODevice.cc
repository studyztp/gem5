#include "dev/bridgeIODevice.hh"

namespace gem5
{

BridgeIODevice::BridgeIODevice(const Params &p)
    : BasicPioDevice(p, p.pio_size),
      interruptPin(p.interrupt_pin),
      inputData(p.input_data_buffer_size, 0),
      inputDataBufferSize(p.input_data_buffer_size),
      outputData(p.output_data_buffer_size, 0),
      outputDataBufferSize(p.output_data_buffer_size)
{
    panic_if(inputDataBufferSize + outputDataBufferSize + 6 > pioSize,
        "PIO size too small for BridgeIODevice data buffers\n");
    // setup the registers
    if (p.go) {
        registers[0] = 1; // set the 'go' register
    }
    DPRINTF(BridgeIO, "BridgeIODevice ctor: pio_addr=%#llx pio_size=%u "
            "in_buf=%u out_buf=%u\n",
            (unsigned long long)p.pio_addr, (unsigned)p.pio_size,
            (unsigned)p.input_data_buffer_size,
            (unsigned)p.output_data_buffer_size);
    registers[1] = 0; // initially not 'done'
    // the second register is 'done' status
    // the third is the address of input data buffer
    registers[2] = p.pio_addr + 6 * sizeof(RegSize);
    inputDataStartAddr = registers[2];
    inputDataEndAddr = inputDataStartAddr + inputDataBufferSize;
    // the fourth is the size of input data buffer
    registers[3] = 0;
    // the fifth is the address of output data buffer
    registers[4] = inputDataEndAddr;
    outputDataStartAddr = inputDataEndAddr;
    outputDataEndAddr = outputDataStartAddr + outputDataBufferSize;
    // the sixth is the size of output data buffer
    registers[5] = 0;

    outputData.resize(outputDataBufferSize, 0);
    inputData.resize(inputDataBufferSize, 0);

    if (p.isa == "Arm") {
        ISA = ISAType::Arm;
    } else if (p.isa == "Riscv") {
        ISA = ISAType::Riscv;
    } else {
        ISA = ISAType::Unknown;
    }

    // init signal handler for SIGUSR1
    installSignalHandler(SIGUSR1, [](int signum) {
        BridgeIODevice::handleSignal(signum);
    });
}

Tick BridgeIODevice::read(PacketPtr pkt)
{
    Addr addr = pkt->getAddr();
    const Addr regs_bytes = RegisterSize * sizeof(RegSize);

    /* Registers region: [pioAddr, pioAddr + regs_bytes) */
    if (addr >= pioAddr && addr < pioAddr + regs_bytes) {
        panic_if(pkt->getSize() != sizeof(RegSize),
                 "Register reads must be of size %llu in BridgeIODevice\n",
                 sizeof(RegSize));
        Addr offset = addr - pioAddr;
        size_t idx = offset / sizeof(RegSize);
    DPRINTF(BridgeIO, "Register read: idx=%u addr=%#llx size=%u val=%#llx\n",
        (unsigned)idx, (unsigned long long)addr, (unsigned)pkt->getSize(),
        (unsigned long long)registers[idx]);
        pkt->setData(reinterpret_cast<uint8_t *>(&registers[idx]));
        pkt->makeResponse();
        return pioDelay;
    }

    /* Input data buffer region (absolute addresses) */
    if (addr >= inputDataStartAddr && addr < inputDataEndAddr) {
        size_t size = pkt->getSize();
        Addr offset = addr - inputDataStartAddr;
        panic_if(offset + size > inputDataBufferSize,
                 "Read size out of range in input buffer\n");
    DPRINTF(BridgeIO, "Input buffer read: offset=%#llx size=%u\n",
        (unsigned long long)offset, (unsigned)size);
        pkt->setData(&inputData[offset]);
        pkt->makeResponse();
        return pioDelay;
    }

    panic("Read address out of range in BridgeIODevice::read\n");
}

Tick BridgeIODevice::write(PacketPtr pkt)
{
    Addr addr = pkt->getAddr();
    const Addr regs_bytes = RegisterSize * sizeof(RegSize);

    /* Register writes */
    if (addr >= pioAddr && addr < pioAddr + regs_bytes) {
        panic_if(pkt->getSize() != sizeof(RegSize),
                 "Register writes must be of size %llu in BridgeIODevice\n",
                 sizeof(RegSize));
        Addr offset = addr - pioAddr;
        size_t idx = offset / sizeof(RegSize);
        pkt->writeData(reinterpret_cast<uint8_t *>(&registers[idx]));
    DPRINTF(BridgeIO, "Register write: idx=%u addr=%#llx size=%u val=%#llx\n",
        (unsigned)idx, (unsigned long long)addr, (unsigned)pkt->getSize(),
        (unsigned long long)registers[idx]);
        pkt->makeResponse();
        if (idx == 1 && registers[1] == 1) {
            exitSimLoopNow("BridgeIODevice signaled done.");
        }
        return pioDelay;
    }

    /* Writing to output data buffer */
    if (addr >= outputDataStartAddr && addr < outputDataEndAddr) {
        size_t size = pkt->getSize();
        Addr offset = addr - outputDataStartAddr;
        panic_if(offset + size > outputDataBufferSize,
                 "Write size out of range in output buffer\n");
    pkt->writeData(&outputData[offset]);
    DPRINTF(BridgeIO, "Output buffer write: offset=%#llx size=%u\n",
        (unsigned long long)offset, (unsigned)size);
        pkt->makeResponse();
        return pioDelay;
    }

    panic("Write address out of range in BridgeIODevice::write\n");
}

bool BridgeIODevice::updateDone(bool done)
{
    registers[1] = done ? 1 : 0;
    DPRINTF(BridgeIO, "updateDone: done=%d\n", registers[1]);
    return true;
}

bool BridgeIODevice::updateInputData(const std::vector<uint8_t> &data)
{
    if (!registers[0]) {
        return false; // device is not on
    }
    if (data.size() > inputDataBufferSize) {
        return false; // data size exceeds buffer size
    }
    inputData.assign(data.begin(), data.end());
    registers[3] = data.size(); // update input data size register
    DPRINTF(BridgeIO, "updateInputData: size=%u inputDataBufferSize=%u\n",
            (unsigned)data.size(), (unsigned)inputDataBufferSize);
    return true;
}

bool BridgeIODevice::raiseInterrupt()
{
    if (!registers[0]) {
        return false; // device is not on
    }
    if (interruptPin) {
        // Raise the interrupt signal
        if (ISA == ISAType::Arm) {
            // Common Python-side configuration often passes an ArmSPIGen
            // (generator) object rather than a concrete ArmSPI instance.
            // Try both: if interruptPin is an ArmSPI, use it directly.
            // Otherwise, if it's an ArmSPIGen, call get() to obtain the
            // ArmInterruptPin and use that.
            ArmSPI *spi = dynamic_cast<ArmSPI *>(interruptPin);
            if (spi) {
                spi->raise();
                    DPRINTF(BridgeIO, "raiseInterrupt: raised via ArmSPI\n");
            } else {
                ArmSPIGen *gen = dynamic_cast<ArmSPIGen *>(interruptPin);
                if (gen) {
                    ArmInterruptPin *pin = gen->get();
                    ArmSPI *spi2 = dynamic_cast<ArmSPI *>(pin);
                    if (!spi2) {
                        panic("BridgeIODevice: ArmSPIGen did not produce an "
                                                                    "ArmSPI");
                    }
                    spi2->raise();
                } else {
                    panic("BridgeIODevice: interrupt_pin is not an ArmSPI or "
                                                "ArmSPIGen (misconfigured)");
                }
            }
        } else if (ISA == ISAType::Riscv) {
            // RISC-V not implemented here
            assert(false && "RISC-V interrupt support not implemented");
        } else {
            assert(false && "Unsupported ISA for BridgeIODevice interrupt");
        }
        return true;
    }
    return false; // no interrupt pin connected
}

bool BridgeIODevice::clearInterrupt()
{
    if (interruptPin) {
        // Clear the interrupt signal
        if (ISA == ISAType::Arm) {
            ArmSPI *spi = dynamic_cast<ArmSPI *>(interruptPin);
            if (spi) {
                spi->clear();
                    DPRINTF(BridgeIO, "clearInterrupt: cleared via ArmSPI\n");
            } else {
                ArmSPIGen *gen = dynamic_cast<ArmSPIGen *>(interruptPin);
                if (gen) {
                    ArmInterruptPin *pin = gen->get();
                    ArmSPI *spi2 = dynamic_cast<ArmSPI *>(pin);
                    if (!spi2) {
                        panic("BridgeIODevice: ArmSPIGen did not produce an "
                                                                    "ArmSPI");
                    }
                    spi2->clear();
                } else {
                    panic("BridgeIODevice: interrupt_pin is not an ArmSPI or "
                                                "ArmSPIGen (misconfigured)");
                }
            }
        } else if (ISA == ISAType::Riscv) {
            assert(false && "RISC-V interrupt support not implemented");
        } else {
            assert(false && "Unsupported ISA for BridgeIODevice interrupt");
        }
        return true;
    }
    return false; // no interrupt pin connected
}

bool BridgeIODevice::ifDone() const
{
    return registers[1] == 1;
}

std::vector<uint8_t> BridgeIODevice::getOutputData() const
{
    return outputData;
}

int BridgeIODevice::getOutputDataSize() const
{
    return static_cast<int>(registers[5]);
}

void BridgeIODevice::handleSignal(int signum)
{
    if (signum != SIGUSR1) {
        panic("BridgeIODevice received unexpected signal: %d.", signum);
    }
    exitSimLoopNow("BridgeIODevice received SIGUSR1 signal.");
}

} // namespace gem5
