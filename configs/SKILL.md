# Example Configurations - Navigation

## What This Directory Contains
Example simulation configuration scripts for various use cases. These are the scripts
users run with `gem5.opt configs/example/...` to set up and run simulations.

## Key Subdirectories

| Path | Purpose |
|------|---------|
| `example/` | Main example configs: `se.py` (syscall emulation), `fs.py` (full system), `gem5_library/` (standard library examples). |
| `example/gem5_library/` | Modern configs using gem5 stdlib: `arm-hello.py`, `arm-ubuntu-run.py`, `arm-demo-ubuntu-run.py`, `x86-ubuntu-run.py`, etc. |
| `common/` | Shared configuration helpers: `Options.py`, `Simulation.py`, `CacheConfig.py`, `MemConfig.py`, `FSConfig.py`. |
| `common/cores/arm/` | ARM core configurations: `O3_ARM_v7a.py` (detailed ARMv7-A OoO config), `O3_ARM_Etrace.py`. |
| `learning_gem5/` | Tutorial configs: `part1/simple-arm.py`, `part1/simple.py`, etc. |
| `boot/` | Boot-related configs and scripts. |
| `ruby/` | Ruby coherence protocol configs. |
| `topologies/` | Network topology definitions (Mesh, Crossbar, etc.). |
| `dram/` | DRAM-specific test configs. |
| `nvm/` | Non-volatile memory configs. |
| `splash2/` | SPLASH-2 benchmark configs. |
| `network/` | Network simulation configs. |
| `deprecated/` | Deprecated/legacy configs. |
| `dist/` | Distributed simulation configs. |

## ARM-Specific Configs

| File | Purpose |
|------|---------|
| `example/gem5_library/arm-hello.py` | Simple ARM SE hello-world with TimingSimpleCPU. |
| `example/gem5_library/arm-ubuntu-run.py` | ARM Ubuntu full-system run. |
| `example/gem5_library/arm-ubuntu-run-with-kvm.py` | ARM Ubuntu with KVM acceleration. |
| `example/gem5_library/arm-demo-ubuntu-run.py` | ARM demo Ubuntu. |
| `common/cores/arm/O3_ARM_v7a.py` | Detailed ARMv7-A core config. |
| `learning_gem5/part1/simple-arm.py` | Learning tutorial ARM config. |

## Relevance to M-Profile
- No M-profile configs exist. New configs would be added here (e.g.,
  `example/gem5_library/arm-m-profile-hello.py`).
- `common/cores/arm/` could have a Cortex-M core config.
- The stdlib-based configs in `example/gem5_library/` show the modern pattern.

## Where to Look Next

| Task | Go To |
|------|-------|
| Create M-profile example config | `example/gem5_library/` (follow `arm-hello.py` pattern) |
| ARM core configuration details | `common/cores/arm/` |
| Understand SE mode setup | `example/se.py`, `common/Options.py` |
| Understand FS mode setup | `example/fs.py`, `common/FSConfig.py` |
