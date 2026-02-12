# systemc/ — SystemC Integration Context

> **Purpose:** Integration layer allowing gem5 to work with SystemC models. Enables co-simulation between gem5 components and SystemC TLM (Transaction Level Modeling) components.

## Overview

This module provides:
- A SystemC kernel implementation that runs within gem5's event loop
- TLM-2.0 adapters to bridge gem5 ports with SystemC TLM sockets
- Wrappers for using SystemC modules as gem5 SimObjects

## Key Concepts

- **TLM-to-gem5 bridge** — converts SystemC TLM transactions to gem5 Packets
- **gem5-to-TLM bridge** — converts gem5 Packets to SystemC TLM transactions
- Synchronization between gem5's event queue and SystemC's simulation kernel
- Useful for integrating vendor IP models (often distributed as SystemC)

## When to Use

- Integrating third-party SystemC models into gem5 simulation
- Co-simulating with SystemC-based verification environments
- Using TLM-based memory models or peripherals
