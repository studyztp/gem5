# gem5 Simulator - Top-Level Navigation

## What This Directory Contains
gem5 is a modular computer architecture simulator. This is the top-level directory containing
the build system, source code, configurations, tests, and utilities.

## Key Files and Directories

| Path | Purpose |
|------|---------|
| `SConstruct` | Top-level SCons build entry point. Handles build options, Kconfig, and recursive SConscript invocation. |
| `build_opts/` | Predefined build configurations (e.g., `ARM`, `X86`, `RISCV`). Each file is a defconfig for Kconfig. |
| `build_tools/` | Build helpers: `marshal.py` (Python embedding), `infopy.py`, `kconfig_base.py`. |
| `src/` | **Core source tree** — all C++ and Python simulation code. See `src/SKILL.md`. |
| `configs/` | Example simulation configurations for SE and FS mode. See `configs/SKILL.md`. |
| `ext/` | External dependencies: pybind11, googletest, softfloat, libelf, dramsim3, systemc, etc. |
| `tests/` | Regression tests (`tests/gem5/`), unit tests (`tests/pyunit/`), and `run.py` driver. |
| `util/` | Tools: `m5/` (magic instructions), `tlm/`, `statetrace/`, `dockerfiles/`, `style/`. |
| `system/` | Optional system software (bootloaders, firmware) for simulated systems. |
| `site_scons/` | SCons extensions: builders, kconfig integration, source tracking. |
| `include/` | Public C++ headers for embedding gem5. |

## Build System
- **Tool**: SCons with Python 3.6+
- **Build command**: `scons build/<CONFIG>/gem5.opt` (e.g., `build/ARM/gem5.opt`)
- **Config selection**: via `build_opts/` defconfigs or Kconfig (`scons menuconfig`)
- **Output variants**: `gem5.debug`, `gem5.opt`, `gem5.fast`

## Where to Look Next

| Task | Go To |
|------|-------|
| Modify CPU, ISA, memory, or device models | `src/SKILL.md` |
| Add or change ARM architecture support | `src/arch/arm/SKILL.md` |
| Change simulation configurations | `configs/SKILL.md` |
| Modify build options for an ISA | `build_opts/ARM` (or other ISA) |
| Add external library dependency | `ext/` |
| Write or modify tests | `tests/` |
