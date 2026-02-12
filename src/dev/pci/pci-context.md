# dev/pci/ — PCI Subsystem Context

> **Purpose:** PCI device infrastructure — base classes for PCI devices, host bridges, and PCI bus modeling.

## Key Files

| File | Class | Description |
|------|-------|-------------|
| `device.hh/cc` | `PciDevice` | Base class for all PCI devices. Inherits `DmaDevice`. Manages BAR registers, config space, interrupt pins. |
| `host.hh/cc` | `PciHost` / `GenericPciHost` | PCI host bridge — translates CPU MMIO to PCI config space. Maps PCI BARs to system address space. |
| `bus.hh/cc` | `PciBus` | PCI bus model connecting devices to host. |
| `upstream.hh/cc` | `PciUpstreamPort` | Upstream PCI port. |
| `up_down_bridge.hh/cc` | `PciUpDownBridge` | PCI-to-PCI bridge for hierarchical bus structure. |
| `copy_engine.hh/cc` | `CopyEngine` | Intel I/O AT DMA copy engine (example PCI DMA device). |
| `types.hh` | | PCI type definitions (BARs, capabilities). |
| `pcireg.h` | | PCI register offset constants. |

## PCI Device Hierarchy

```
DmaDevice
  └── PciDevice (pci/device.hh)
      ├── IGbE (net/i8254xGBe.hh)
      ├── IdeController (storage/ide_ctrl.hh)
      ├── CopyEngine (pci/copy_engine.hh)
      ├── VirtIOPciDevice (virtio/pci.hh)
      └── [custom PCI devices]
```

## Adding a New PCI Device

1. Inherit from `PciDevice`
2. Implement `read()`/`write()` for BAR-mapped MMIO
3. Use `dmaRead()`/`dmaWrite()` inherited from `DmaDevice` for DMA
4. Set up config space in Python SimObject (vendor ID, device ID, BARs)
5. Use `intrPost()`/`intrClear()` for interrupt signaling (legacy INTx or MSI)
