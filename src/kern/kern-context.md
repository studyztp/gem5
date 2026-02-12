# kern/ — Kernel Support Context

> **Purpose:** OS-specific helpers for full-system simulation. Provides kernel event hooks and operating system abstractions.

## Key Files

| File | Class | Description |
|------|-------|-------------|
| `operatingsystem.hh/cc` | `OperatingSystem` | Base OS helper — provides OS-specific type sizes and ABI info for SE mode. |
| `system_events.hh/cc` | `PCEvent` subclasses | Kernel event hooks — skip idle loops, handle kernel panics, etc. |

## OS-Specific Directories

| Directory | Description |
|-----------|-------------|
| `linux/` | Linux-specific helpers: `linux_events.hh/cc` (skip idle, panic/oops hooks), `printk.hh/cc` (extract kernel messages). |
| `freebsd/` | FreeBSD-specific helpers. |
| `solaris/` | Solaris-specific helpers. |

## How It Works

- `PCEvent` objects are registered at specific kernel PC addresses
- When the CPU reaches that PC, the event fires
- Used to intercept kernel behaviors: skip useless spinning, detect panics, extract debug info
- Kernel symbols loaded from ELF binary provide PC addresses

## Common Use Cases

- Skip idle loop (FS simulation speedup)
- Detect kernel panic/oops
- Extract printk messages for logging
- Annotate simulation with kernel events
