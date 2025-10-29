
#ifndef __DEV_DUMMY_SPI_HH__
#define __DEV_DUMMY_SPI_HH_

#include "dev/dma_device.hh"
#include "cpu/minor/cpu.hh"
#include "params/DummySPI.hh"
#include "debug/DummySPI.hh"
#include "arch/arm/interrupts.hh"
#include "sim/eventq.hh"

namespace gem5
{
// Dummy SPI device receives stores the data to a memory location via DMA and
// interrupts the target CPU's thread when a transfer is complete.
// After the target CPU services the interrupt, it writes to the SPI device to
// give it the next transfer parameters.
// It is combining both the sensor signal and motion command together for 
// simplicity for now.

struct Position {
    uint64_t x;
    uint64_t y;
};

class DummySPI : public DmaDevice
{
  public:
    PARAMS(DummySPI);
    DummySPI(const Params &p);
  private:
    void performSignaling();
  public:
    Tick read(PacketPtr pkt) override;
    Tick write(PacketPtr pkt) override;
    AddrRangeList getAddrRanges() const override {
      assert(pioSize != 0);
      AddrRangeList ranges;
      DPRINTF(DummySPI, "registering range: %#x-%#x\n", pioAddr, pioSize);
      ranges.push_back(RangeSize(pioAddr, pioSize));
      return ranges;
    }

  private:
    Addr targetAddress;
    MinorCPU *targetCpu;
    ThreadID interruptThreadID;
    int moveDelayCycles;
    void raiseInterrupt() {
        targetCpu->postInterrupt(interruptThreadID, ArmISA::INT_IRQ, 0);
        targetCpu->postInterrupt(0, ArmISA::INT_IRQ, 0);
        targetCpu->setTargetThreadID(interruptThreadID);
        DPRINTF(DummySPI, "Switching to interrupt thread %d\n", 
                                              targetCpu->getTargetThreadID());
    }
    void clearInterrupt() {
        targetCpu->clearInterrupts(interruptThreadID);
        targetCpu->clearInterrupts(0);
        targetCpu->setTargetThreadID(0);
        DPRINTF(DummySPI, "Switching to default thread %d\n", 
                                              targetCpu->getTargetThreadID());
    }
    EventFunctionWrapper interruptEvent;
    EventFunctionWrapper performSignalingEvent;

  private:
    // Some robot simulation specific functions
    uint64_t gridWidth;
    uint64_t gridHeight;
    Position currentPosition;
    uint8_t surroundingObstacles; // 4 bits for 4 directions
    uint8_t command;
    uint8_t getSurroundingSignals() {
        // currently just assume that the grid is a box with no internal
        // obstacles
        uint8_t signals = 0;
        if (currentPosition.y == 0) {
            signals |= 0x1; // north
        }
        if (currentPosition.y == gridHeight - 1) {
            signals |= 0x2; // south
        }
        if (currentPosition.x == 0) {
            signals |= 0x4; // west
        }
        if (currentPosition.x == gridWidth - 1) {
            signals |= 0x8; // east
        }
        return signals;
    }
    void moveToNewPosition(uint8_t command) {
        Position newPosition = currentPosition;
        switch (command) {
          case 0: // north
            if (currentPosition.y > 0)
                newPosition.y -= 1;
            break;
          case 1: // south
            if (currentPosition.y < gridHeight - 1)
                newPosition.y += 1;
            break;
          case 2: // west
            if (currentPosition.x > 0)
                newPosition.x -= 1;
            break;
          case 3: // east
            if (currentPosition.x < gridWidth - 1)
                newPosition.x += 1;
            break;
          default:
            break;
        }
        currentPosition = newPosition;
    }
    std::string drawGrid() {
        std::string grid;
        grid += "\n";
        for (uint64_t y = 0; y < gridHeight; y++) {
            for (uint64_t x = 0; x < gridWidth; x++) {
                if (currentPosition.x == x && currentPosition.y == y) {
                    grid += "R "; // Robot's position
                } else {
                    grid += ". ";
                }
            }
            grid += "\n";
        }
        return grid;
    }

    std::string addrToString(Addr addr) const
    {
        std::stringstream ss;
        ss << std::hex << addr;
        return ss.str();
    }

  private:
    Addr pioAddr;
    Addr pioSize;
    Tick pioDelay;
};

}

#endif // __DEV_DUMMY_SPI_HH__
