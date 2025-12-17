#ifndef __DEV_BRIDGE_IO_DEVICE_HH__
#define __DEV_BRIDGE_IO_DEVICE_HH__

#include <csignal>
#include <string>
#include <vector>

#include "debug/BridgeIO.hh"
#include "dev/arm/base_gic.hh"
#include "dev/io_device.hh"
#include "params/BridgeIODevice.hh"
#include "sim/init_signals.hh"
#include "sim/sim_exit.hh"
#include "sim/sim_object.hh"

namespace gem5
{

using RegSize = uint32_t;
#define RegisterSize 6

// This is a basic template for a Bridge IO Device in gem5.
// It can raise interrupt signals to the platform's interrupt controller.
//
class BridgeIODevice : public BasicPioDevice
{

  enum ISAType
  {
      Arm,
      Riscv,
      Unknown
  };

  public:
    PARAMS(BridgeIODevice);
    BridgeIODevice(const Params &p);

  protected:
    SimObject *interruptPin;  // Pointer to the interrupt pin
    RegSize registers[RegisterSize]; // Device registers
    std::vector<uint8_t> inputData;  // The actual data storage
    size_t inputDataBufferSize = 0; // Size of input data buffer
    std::vector<uint8_t> outputData; // The output data storage
    size_t outputDataBufferSize = 0; // Size of output data buffer
    ISAType ISA;
    Addr inputDataStartAddr;
    Addr outputDataStartAddr;
    Addr inputDataEndAddr;
    Addr outputDataEndAddr;

  protected:
    Tick read(PacketPtr pkt) override;
    Tick write(PacketPtr pkt) override;

  public:
    bool updateDone(bool done);
    bool updateInputData(const std::vector<uint8_t> &data);
    bool raiseInterrupt();
    bool clearInterrupt();
    bool ifDone() const;
    std::vector<uint8_t> getOutputData() const;
    int getOutputDataSize() const;

  private:
    static void handleSignal(int signum);
};

} // namespace gem5

#endif // __DEV_BRIDGE_IO_DEVICE_HH__
