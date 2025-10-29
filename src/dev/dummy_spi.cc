#include "dev/dummy_spi.hh"

namespace gem5
{
DummySPI::DummySPI(const Params &p)
    : DmaDevice(p),
      targetAddress(p.target_address),
      targetCpu(p.target_cpu),
      interruptThreadID(p.interrupt_thread_id),
      moveDelayCycles(p.move_delay_cycles),
      interruptEvent([this]{ raiseInterrupt(); }, name() + ".interruptEvent"),
      performSignalingEvent([this]{ performSignaling(); }, name() + ".performSignalingEvent"),
      gridWidth(p.grid_width), gridHeight(p.grid_height),
      currentPosition({p.start_x, p.start_y}),
      pioAddr(p.pio_addr), pioSize(p.pio_size),
      pioDelay(p.pio_latency)
{
    surroundingObstacles = getSurroundingSignals();
    DPRINTF(DummySPI, "DummySPI initialized at position (%lu, %lu) "
                      "in grid %lux%lu\n",
                      currentPosition.x, currentPosition.y,
                      gridWidth, gridHeight);
    DPRINTF(DummySPI, "Target address: 0x%lx, "
                      "Interrupt thread ID: %d\n",
                      targetAddress, interruptThreadID);
    DPRINTF(DummySPI, "\nStarting Grid: %s", drawGrid().c_str());
}

void DummySPI::performSignaling()
{
    surroundingObstacles = getSurroundingSignals();
    // Prepare DMA to write the surrounding signals to the target address.
    // After DMA is done, it will raise an interrupt to the target CPU.
    dmaWrite(targetAddress, sizeof(surroundingObstacles), &interruptEvent,
             &surroundingObstacles);
    DPRINTF(DummySPI, "Performing signaling: surrounding obstacles = 0x%02x\n",
                      surroundingObstacles);
}

Tick DummySPI::read(PacketPtr pkt)
{
    // When the CPU reads from the SPI device, it means sensor starts.
    // When sensor starts, we need to store the surrounding signals into the
    // memory location for the CPU to read later.
    DPRINTF(DummySPI, "Received sensor start read request\n");
    DPRINTF(DummySPI, "Trigger address is %s\n", 
                                        addrToString(pkt->getAddr()).c_str());
    performSignaling();

    if (pkt->needsResponse() && !pkt->isResponse()) {
        pkt->makeResponse();
    }

    return pioDelay;
}

Tick DummySPI::write(PacketPtr pkt)
{
    DPRINTF(DummySPI, "Received motion command write request\n");
    DPRINTF(DummySPI, "Trigger address is %s\n", 
                                        addrToString(pkt->getAddr()).c_str());
    DPRINTF(DummySPI, "Pkt cmd is %s\n", pkt->cmd.toString().c_str());
    // When the CPU writes to the SPI device, it means motion command.
    // It also means that the interrupt has been serviced, so we can clear it.
    clearInterrupt();
    // Read the command from the packet
    command = pkt->getPtr<uint8_t>()[0];
    // Update robot position based on the command
    moveToNewPosition(command);
    // Schedule next sensor reading after some delay
    schedule(performSignalingEvent, clockEdge(Cycles(moveDelayCycles)));
    DPRINTF(DummySPI, "Received command: 0x%02x, new position: (%lu, %lu)\n",
                      command, currentPosition.x, currentPosition.y);
    DPRINTF(DummySPI, "\nCurrent Grid: %s", drawGrid().c_str());
    DPRINTF(DummySPI, "Scheduling next signaling in %d cycles\n",
                      moveDelayCycles);

    if (pkt->needsResponse()) {
        DPRINTF(DummySPI, "Making pkt response\n");
        pkt->makeResponse();
    }
    DPRINTF(DummySPI, "Pkt isResponse: %s\n", 
                                pkt->isResponse() ? "true" : "false");

    return pioDelay;
}

} // namespace gem5
