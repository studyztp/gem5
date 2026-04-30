# MProfileBridgeIO development log

Log for the Arm-M `MProfileBridgeIO` device + supporting changes.
Plan: `~/.claude/plans/this-code-is-in-vivid-whale.md`.

Format per entry:
- **Date** — ISO date.
- **File(s)** — paths edited.
- **Why** — problem being solved.
- **What** — summary of the change.
- **Interactions / backward compatibility** — what else this touches and why existing behaviour is preserved.

---

## 2026-04-30 — Round 1: Implement `MProfileSCS::sendInt` / `clearInt`

**File(s):** [src/dev/arm/m_profile_scs.cc:817-826](src/dev/arm/m_profile_scs.cc#L817-L826) (replaced).

**Why:** Both methods were empty `// TODO` stubs. They are the public peripheral-side API for raising/clearing external IRQs on the M-profile NVIC. Without an implementation, any device that asks the SCS to raise an IRQ (including the upcoming `MProfileBridgeIO`) is silently dropped.

**What:**
- `sendInt(uint32_t irq)`: translates 0-based external IRQ index to exception number (`irq + 16`, per ARMv7-M B1.5.2), bounds-checks against `interrupts.size()`, and calls the existing `pendInterrupt(...)` primitive — the same call used by ISPR writes ([line 591-592](src/dev/arm/m_profile_scs.cc#L591-L592)), STIR ([line 627](src/dev/arm/m_profile_scs.cc#L627)), and SysTick expiry ([line 1026](src/dev/arm/m_profile_scs.cc#L1026)). `pendInterrupt` sets the pending bit, queues it in the priority heap (idempotently), and wakes the CPU via `postInterrupt`.
- `clearInt(uint32_t irq)`: bounds-checks the same way and sets `interrupts[excNum].pending = false`. This mirrors the ICPR W1C semantics at [line 596-597](src/dev/arm/m_profile_scs.cc#L596-L597). Active state is intentionally not touched (ARMv7-M B3.4.6: ICPR has no effect on active interrupts).
- Both functions `panic_if` on out-of-range IRQ indices so misconfigured devices fail loudly instead of corrupting `interrupts[]` indexing.

**Interactions / backward compatibility:**
- No callers in-tree existed before this change (grep'd) — the stubs were dead code. Filling them in is purely additive.
- `pendInterrupt` guards CPU wakeup with `if (tc)` ([line 891](src/dev/arm/m_profile_scs.cc#L891)), so calling `sendInt` before `tc` is set (e.g., from another device's constructor) is a benign no-op for the wakeup; the pending bit is still recorded.
- Pending priority-queue entries left behind by `clearInt` are not removed — `updatePending`/consumer paths re-check `intr.pending` before delivery, so stale entries are inert. This matches the existing ICPR path's behaviour.

## 2026-04-30 — Round 2: Add `MProfileBridgeIO` device

**File(s):** [src/dev/arm/m_profile_bridge_io.hh](src/dev/arm/m_profile_bridge_io.hh) (new), [src/dev/arm/m_profile_bridge_io.cc](src/dev/arm/m_profile_bridge_io.cc) (new), [src/dev/arm/MProfileBridgeIO.py](src/dev/arm/MProfileBridgeIO.py) (new).

**Why:** Need a bridge device that gives a gem5 Python harness a way to feed input/output data to simulated firmware and raise external IRQs on the M-profile NVIC. The reference generic `BridgeIODevice` from gem5 commit `0d8839ea` uses A-profile GIC types (`ArmSPI`/`ArmSPIGen`) for its interrupt path, which don't apply on Cortex-M.

**What:**
- `MProfileBridgeIO : public BasicPioDevice` with the same MMIO layout as the reference (6 32-bit control registers + input buffer + output buffer; firmware writing 1 to `register[1]` calls `exitSimLoopNow`).
- Interrupt path replaced with `MProfileSCS *scs` + `uint32_t irqNum`. `raiseInterrupt()` calls `scs->sendInt(irqNum)`; `clearInterrupt()` calls `scs->clearInt(irqNum)` — both implemented in Round 1.
- `init()` does a bounds check against `scs->params().num_irqs` so a misconfigured `irq_num` fails at config time with a clear `fatal_if` message.
- PyBind-exported methods (`updateDone`, `raiseInterrupt`, `updateInputData`, `clearInterrupt`, `ifDone`, `getOutputData`, `getOutputDataSize`) match the reference exactly so a Python harness can be reused with a single class-name change.
- No SIGUSR1 handler (skipped per the design decision in the planning round — keeps `dumpStatsHandler` intact and avoids the `installSignalHandler` refactor the reference commit needed).

**Interactions / backward compatibility:**
- All three files are new. Nothing outside `src/dev/arm/` is touched.
- The device is only built once Round 3 wires `m_profile_bridge_io.cc` and `MProfileBridgeIO.py` into [src/dev/arm/SConscript](src/dev/arm/SConscript). Until then, the build is unchanged.
- Depends on the Round 1 `sendInt`/`clearInt` impl; without it `raiseInterrupt`/`clearInterrupt` would be no-ops.
- Reads `scs->params().num_irqs` in `init()`. Cross-SimObject `params()` access is a standard gem5 pattern (e.g., [src/dev/arm/gic_v3_redistributor.cc:80](src/dev/arm/gic_v3_redistributor.cc#L80) does the same with `gic->params().gicv4`).
- `exitSimLoopNow` from `write()` schedules an exit event at `curTick()` rather than unwinding, so the packet response is delivered cleanly first ([sim/sim_exit.hh:56](src/sim/sim_exit.hh#L56)).

## 2026-04-30 — Round 3: Register `MProfileBridgeIO` in the ARM SConscript

**File(s):** [src/dev/arm/SConscript](src/dev/arm/SConscript) — three additions (no replacements).

**Why:** Without SCons registration, `m_profile_bridge_io.cc` is not compiled and `m5.objects.MProfileBridgeIO` does not exist at runtime.

**What:**
- New `SimObject('MProfileBridgeIO.py', sim_objects=['MProfileBridgeIO'], tags=['arm isa'])` block immediately after the `MProfileSCS` block.
- New `Source('m_profile_bridge_io.cc', tags=['arm isa'])` immediately after `m_profile_scs.cc`.
- New `DebugFlag('MProfileBridgeIO', tags=['arm isa'])` immediately after `DebugFlag('MProfileSCS', ...)`.

**Interactions / backward compatibility:**
- All entries tagged `'arm isa'`, matching the file-wide convention enforced by the `Return()` at line 41 for non-ARM builds. Non-ARM build configurations are unaffected.
- No existing entry is changed; only additions, so existing devices/source files build identically.

## 2026-04-30 — Round 4: Wire `MProfileBridgeIO` into `STM32G474RETimingBoard`

**File(s):** [src/python/gem5/prebuilt/cortexm/boards/stm32g474re_board.py](src/python/gem5/prebuilt/cortexm/boards/stm32g474re_board.py).

**Why:** Without board integration, every user has to wire the bridge by hand. Making it an opt-in kwarg keeps existing configs unaffected while giving callers a one-line way to enable it.

**What:**
- `__init__` gains three kwargs: `enable_bridge_io=False`, `bridge_pio_addr=0x90000000`, `bridge_irq_num=101`. New docstring section documents each one (defaults + rationale).
- New conditional block placed immediately after the SCS wiring (formerly [line 335](src/python/gem5/prebuilt/cortexm/boards/stm32g474re_board.py#L335)). When `enable_bridge_io` is True, it does a lazy `from m5.objects.MProfileBridgeIO import MProfileBridgeIO`, instantiates the device with `scs=self.platform.scs`, and connects `self.bridge_io.pio = self.system_bus.mem_side_ports`.

**Interactions / backward compatibility:**
- `enable_bridge_io` defaults to False. Every existing call site (`STM32G474RETimingBoard()`, `STM32G474RETimingBoard(clk_freq=...)`, etc.) sees byte-identical behaviour: no new attribute is added, no new SimObject is constructed, no new bus connection is made.
- The `from m5.objects.MProfileBridgeIO` import is lazy — kept inside the `if`-block — so a half-built tree (e.g., the SimObject not yet registered in SConscript) still imports the board module cleanly when `enable_bridge_io` is False.
- `bridge_pio_addr=0x90000000` is outside `periph_ranges` (`0x40000000+512MiB` per [platforms.py:248](src/python/gem5/prebuilt/cortexm/platforms.py#L248)) and outside the flash/SRAM/CCM/SCS ranges, so the platform's BadAddr responder cannot collide with it.
- `bridge_irq_num=101` is the highest valid index for `STM32G474REPlatform` (num_irqs=102 per [platforms.py:261](src/python/gem5/prebuilt/cortexm/platforms.py#L261)). Bounds-checked in `MProfileBridgeIO::init()` (Round 2).
- Bridge is independent of `enable_art`: both code paths terminate with the AHB `system_bus` having `mem_side_ports` available for new peripherals.

## 2026-04-30 — Round 5: End-to-end smoke test

**File(s):** [tests/gem5/m_profile_tests/programs/test_bridge_io.S](tests/gem5/m_profile_tests/programs/test_bridge_io.S) (new), [tests/gem5/m_profile_tests/programs/Makefile](tests/gem5/m_profile_tests/programs/Makefile) (append `test_bridge_io` to TESTS), [tests/gem5/m_profile_tests/configs/run_bridge_io_test.py](tests/gem5/m_profile_tests/configs/run_bridge_io_test.py) (new), [tests/gem5/m_profile_tests/test_m_profile.py](tests/gem5/m_profile_tests/test_m_profile.py) (new helper + registration).

**Why:** The build only proves the device compiles. We need a runtime check that:
- the bridge is mapped into the system bus at the expected address,
- registers[0]/[2]/[4] return the values the constructor set,
- the output buffer region is writable / read-back-coherent,
- writing 1 to registers[1] actually triggers `exitSimLoopNow`.

**What:**
- `test_bridge_io.S` — bare-metal Cortex-M4 firmware that reads `registers[0]` (expects 1), checks `registers[2]==0x90000018` and `registers[4]==0x90000418` against the constructor formula, writes `0xDEADBEEF` to the output buffer and reads it back, then writes `0xCAFECAFE` to the project-wide pass marker (`0x20000100`) and `1` to `registers[1]`. A semihosted `SYS_EXIT` follows as a fallback if the bridge's exit path is broken.
- `Makefile` — adds `test_bridge_io` to the `TESTS` list so `make` picks it up.
- `run_bridge_io_test.py` — instantiates `STM32G474RETimingBoard(enable_bridge_io=True)`, runs to completion, and parses `simout.txt` for both the standard `0xCAFECAFE @ 0x20000100` store and the literal `"MProfileBridgeIO signaled done."` exit message. Both must be present to pass; differentiated failure messages distinguish "PASS marker present but bridge done not seen" from "no PASS marker".
- `test_m_profile.py` — adds a new `m_profile_bridge_io_test()` helper (parallel to `m_profile_checkpoint_test()`) that uses the new run config, with a verifier requiring the bridge done message regex.

**Interactions / backward compatibility:**
- All four edits live entirely under `tests/gem5/m_profile_tests/`. None of the production source files are touched.
- The Makefile change is additive (appends to `TESTS`); existing `make` invocations rebuild the same set of targets plus the new one.
- `test_m_profile.py` change adds a new helper + registration after the existing `m_profile_checkpoint_test()` registration; existing test entries are unchanged.
- The new run config follows the same parser shape as `run_timing_board_test.py` (accepts `--firmware`, `--tick-limit`, plus an ignored `--expected-result` so the existing `m_profile_test()` helper would also work if desired).
- Tests run via the standard command per the saved feedback memory: `python3 tests/main.py run tests/gem5/m_profile_tests/ --length quick --skip-build`.

**Future work (deliberately deferred):**
- IRQ end-to-end: a test that calls `bridge.raiseInterrupt()` from Python while the simulation is paused, then resumes and asserts an ISR ran. Requires Python-side `m5.simulate(...)` pause/resume scaffolding plus an ISR registration in firmware. Substantially more harness code; better as a separate change once the smoke test is green.
- `clearInterrupt()` exercise: same pause/resume requirement.

## 2026-04-30 — Round 6: Revert board kwargs, rely on caller-side bridge attachment

**File(s):** [src/python/gem5/prebuilt/cortexm/boards/stm32g474re_board.py](src/python/gem5/prebuilt/cortexm/boards/stm32g474re_board.py) (revert Round 4), [tests/gem5/m_profile_tests/configs/run_bridge_io_test.py](tests/gem5/m_profile_tests/configs/run_bridge_io_test.py) (move bridge wiring out of the board into the test config).

**Why:** Baking `enable_bridge_io` / `bridge_pio_addr` / `bridge_irq_num` into the board constructor restricted callers to a tiny knob set (no buffer sizes, no `go` initial state, no multi-bridge use). It also diverged from the gem5 idiom of attaching peripherals on the caller side (e.g., `board.semihosting = ArmSemihosting()` in `run_timing_board_test.py`).

**What:**
- Reverted `STM32G474RETimingBoard.__init__` signature to `(self, clk_freq="170MHz", enable_art=True)`. Removed the bridge docstring section and the `if enable_bridge_io:` instantiation block.
- Replaced the in-board block with a comment block showing the canonical caller-side attachment recipe (uses the already-public `board.platform.scs` and `board.system_bus.mem_side_ports`).
- `run_bridge_io_test.py` now imports `MProfileBridgeIO`, parses optional `--bridge-pio-addr` and `--bridge-irq-num` flags, instantiates the bridge after `set_workload()`, and wires it to the system bus. Behaviour of the smoke test is unchanged (same default address `0x90000000`, same IRQ 101).

**Interactions / backward compatibility:**
- The board kwargs added in Round 4 had no in-tree callers other than `run_bridge_io_test.py` itself (updated in the same edit), so no external consumers can be affected.
- `board.platform.scs` and `board.system_bus` were already public-facing attributes used by the board's own `__init__` body, so exposing them externally is no new surface area.
- The crazyflie script (and any other harness mirroring its pattern) needs no API change beyond the bridge-class swap and the exit-message string change already documented for the user.

## 2026-04-30 — Round 7: IRQ-driven echo test (mirrors crazyflie ping-pong)

**File(s):** [tests/gem5/m_profile_tests/programs/test_bridge_io_irq.S](tests/gem5/m_profile_tests/programs/test_bridge_io_irq.S) (new), [tests/gem5/m_profile_tests/configs/run_bridge_io_irq_test.py](tests/gem5/m_profile_tests/configs/run_bridge_io_irq_test.py) (new), [tests/gem5/m_profile_tests/programs/Makefile](tests/gem5/m_profile_tests/programs/Makefile) (append `test_bridge_io_irq`), [tests/gem5/m_profile_tests/test_m_profile.py](tests/gem5/m_profile_tests/test_m_profile.py) (new helper + registration).

**Why:** The Round 5 smoke test only validates static device behaviour (register reads, output write, done-on-write exit). It doesn't exercise the API surface the crazyflie harness depends on: multi-iteration `updateInputData()` → `raiseInterrupt()` → `simulate()` → `getOutputData()` → `clearInterrupt()` round trips. Without this, the integration with `MProfileSCS::sendInt` and the IRQ delivery path is unverified end-to-end.

**What:**
- Firmware (`test_bridge_io_irq.S`): vector table sized for IRQs 0..101 with the bridge handler at the IRQ-101 slot (offset 0x1D4). `Reset_Handler` enables IRQ 101 in `NVIC->ISER[3]`, writes the 4-byte sentinel `"INIT"` to the output buffer, sets out_size = 4, signals done, then parks in `cpsie i; wfi; b .` waiting for the IRQ. `Bridge_IRQ_Handler` reads in_size/in_addr/out_addr from the bridge registers, byte-copies input → output, sets out_size, writes done = 1 (triggers exit), and returns via `bx lr`.
- Python harness (`run_bridge_io_irq_test.py`): builds the board with the bridge attached caller-side (Round 6 idiom), runs iter 0 to validate the boot path (output must be `"INIT"`), then loops three iterations of `updateDone(False) → updateInputData(...) → raiseInterrupt() → simulate(per_iter_tick_limit) → clearInterrupt() → assert getOutputData() == input`. On the first mismatch it prints a diagnostic and exits non-zero; on full success it prints `"TEST PASSED"` and exits 0.
- Makefile change is purely additive (appends to `TESTS`).
- New `m_profile_bridge_io_irq_test()` helper in `test_m_profile.py` registers the test using the new run config; verifier matches the literal string `"TEST PASSED"` from the Python output.

**Interactions / backward compatibility:**
- All four edits live entirely under `tests/gem5/m_profile_tests/`. Production source is untouched.
- The new test lives alongside existing tests; the Round 5 smoke test (`test_bridge_io`) is unchanged.
- Firmware uses default IRQ priority 0 (highest configurable) so it preempts thread mode without an IPR write. Default priority for external IRQs is set by `MProfileSCS::resetAllInterrupts()`.
- IRQ 101 was chosen to match the bridge default in `run_bridge_io_test.py` and the `STM32G474REPlatform` upper bound (`num_irqs = 102`). If those defaults change, the firmware's `IRQ101_BIT` constant and vector table layout must change in lockstep — flagged in the .S comments.
- `bytes(list(raw)[:n])` in the Python harness handles whichever pybind exposure of `std::vector<uint8_t>` is in effect (sequence-of-int or list-of-int) without depending on a specific binding flavour.

**What this validates that Round 5 didn't:**
- `MProfileSCS::sendInt` actually pends and delivers an external IRQ to the M-profile CPU end-to-end (Round 1 implementation).
- `raiseInterrupt()` correctly routes through `scs->sendInt(irqNum)` and the CPU takes the vector.
- The handler's exception-return path (`bx lr` → unstack → resume thread mode) works after a Python-driven IRQ.
- `clearInterrupt()` is callable without crashing post-handler.
- `updateInputData()` puts bytes where the firmware can read them, and `getOutputData()` reads bytes the firmware wrote — both across multiple iterations and varying payload sizes.
- Multi-shot `exitSimLoopNow` from inside the bridge `write()` works (one boot exit + N iteration exits without state corruption).
