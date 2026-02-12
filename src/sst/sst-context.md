# sst/ — SST Integration Context

> **Purpose:** Integration with the Structural Simulation Toolkit (SST). Allows gem5 CPU models to be used as components within SST simulations, particularly for memory system studies.

## Overview

SST (Structural Simulation Toolkit) is a parallel discrete event simulation framework. The gem5-SST bridge allows:

- gem5 CPU cores to drive SST memory system components
- SST to manage simulation scheduling across multiple components
- Combining gem5's detailed CPU models with SST's scalable infrastructure

## Key Concepts

- gem5 CPUs communicate via SST's link/port mechanism
- Memory requests are translated between gem5 Packets and SST memory events
- Useful for large-scale simulations (many-core, system-level)

## When to Use

- Large-scale multi-node simulations
- Using SST's memory models (Messier, CramSim) with gem5 CPUs
- Network-on-chip studies with SST's Merlin network model
