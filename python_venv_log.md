# Python venv runtime detection log

Log for the runtime virtualenv detection added to `gem5.opt`.
Plan: `~/.claude/plans/this-code-is-in-vivid-whale.md`.

Format per entry:
- **Date** — ISO date.
- **File(s)** — paths edited.
- **Why** — problem being solved.
- **What** — summary of the change.
- **Interactions / backward compatibility** — what else this touches and why existing behaviour is preserved.

---

## 2026-04-30 — Round 1: Validate VIRTUAL_ENV and route into PyConfig.program_name

**File(s):** [src/sim/main.cc](src/sim/main.cc).

**Why:** gem5's embedded Python init at [src/sim/main.cc:67-79](src/sim/main.cc#L67-L79) sets `program_name = argv[0]`, so `sys.prefix` is derived from the gem5 binary's location and `VIRTUAL_ENV` is ignored. Users had to manually `export PYTHONPATH=<venv>/lib/python3.X/site-packages` for every gem5 run that needed a venv-installed package. A simple "always trust VIRTUAL_ENV" fix is dangerous: if the env var is stale (the venv was deleted, or it points at a half-built directory), `Py_Initialize()` aborts with a cryptic error inside libpython rather than producing an actionable diagnostic.

**What:**
- New anonymous-namespace helper `detectVenvProgramName()`:
  - Opt-out: returns `""` immediately if `GEM5_IGNORE_VENV` is set non-empty.
  - Returns `""` if `VIRTUAL_ENV` is unset/empty (caller falls back to `argv[0]`, preserving historical behaviour byte-for-byte).
  - Validates the venv via two `std::filesystem` checks:
    1. `<VIRTUAL_ENV>/pyvenv.cfg` is a regular file (PEP 405 marker).
    2. `<VIRTUAL_ENV>/bin/python` exists (the path Python uses for sys.prefix discovery).
  - On either check failing: emits a `warn(...)` quoting the offending `VIRTUAL_ENV` value, names the missing file, mentions the `GEM5_IGNORE_VENV` opt-out for the first failure, and returns `""`.
  - On success: returns `<venv>/bin/python` for the caller to hand to `PyConfig_SetBytesString`.
- `main()` now calls the helper before `PyConfig_SetBytesString` and substitutes the helper's result for `argv[0]` whenever it's non-empty. When it's empty (no venv / opted-out / invalid venv), the original `argv[0]` is used and `PyConfig_SetBytesString` sees the same value it always did.
- Added `<cstdlib>` (getenv), `<string>` (std::string), and the conditional `<filesystem>` / `<experimental/filesystem>` include shim — copied verbatim from [src/base/socket.cc:43-57](src/base/socket.cc#L43-L57) so this still builds on the GCC 7 / Clang 6-10 toolchains gem5 supports.
- Added `#include "base/logging.hh"` for the `warn(...)` macro.

**Interactions / backward compatibility:**
- **Default behaviour unchanged.** Without `VIRTUAL_ENV` set, the only added work is two `std::getenv` calls and an early return. `program_name` is `argv[0]` exactly as before, so every existing test, config script, and tool sees byte-identical behaviour.
- **Stale-venv case.** Replaces "Python init crashes inside libpython" with "warning printed, simulation runs against compiled-in Python". The warning text quotes the bad `VIRTUAL_ENV` value, names the missing file, and points at `GEM5_IGNORE_VENV` for users who can't easily unset the variable.
- **`warn()` is safe before `Py_Initialize`.** It depends only on `std::iostream` and the static `Logger` getters in [base/logging.hh](src/base/logging.hh), both of which are independent of Python.
- **Python version match still required.** A 3.10-built gem5 pointed at a 3.12 venv via `VIRTUAL_ENV` will still crash inside libpython (compiled-in libpython is 3.10). Out of scope for this round; `python -m venv` venvs created from the same Python that built gem5 always match.
- **No SCons-side change.** The build pipeline is untouched. `PYTHON_CONFIG` overrides still work for the conda-env case where you want a different libpython linked in.
- **Conda not addressed.** Conda envs use `CONDA_PREFIX` and don't have `pyvenv.cfg` at the root; this change leaves them on the existing path. Adding a second branch is a small follow-up if needed.

**Verification (after rebuild):**
1. **No-venv smoke** — `unset VIRTUAL_ENV; gem5.opt <existing test>` → identical output to a pre-change build.
2. **Active venv** — write a 5-line config that prints `sys.prefix` / `sys.executable` / `sys.path[:3]`. With venv active, expect `sys.prefix == <venv>` and the venv site-packages near the front of sys.path.
3. **Stale VIRTUAL_ENV** — `VIRTUAL_ENV=/tmp/no-such-dir gem5.opt …` → `pyvenv.cfg not found` warning on stderr, sim runs to completion.
4. **Half-baked venv** — `mkdir /tmp/empty; touch /tmp/empty/pyvenv.cfg; VIRTUAL_ENV=/tmp/empty gem5.opt …` → `bin/python missing` warning, sim runs.
5. **Opt-out** — venv active + `GEM5_IGNORE_VENV=1` → `sys.prefix` matches build-time Python.
6. **Real import test** — venv active with `pip install`'d package not in system, gem5 config does `import that_package` → succeeds (would fail before this change).

## 2026-04-30 — Round 2: Route the populated PyConfig into pybind11's interpreter init

**File(s):** [src/sim/main.cc](src/sim/main.cc).

**Why:** Round 1's runtime test showed `sys.prefix=/usr` even with `VIRTUAL_ENV=<real venv>` set — the venv was not being honoured. Root cause: the existing `py::scoped_interpreter guard(true, argc, argv);` at [main.cc:179](src/sim/main.cc#L179) was using pybind11's `bool`-overload constructor, which calls `Py_InitializeFromConfig(...)` with its own freshly-default-initialised PyConfig, throwing away the populated `config` we built up. The previous codebase's `PyConfig_SetBytesString(&config, &config.program_name, argv[0])` was already silently dead — `program_name` was always coming from libpython's compile-time discovery. This was the same reason the warnings for stale/half-baked venvs weren't appearing in the smoke output: detection ran but its effect (and any side-effect warnings emitted earlier in main()) was being routed through a path that did nothing observable to `sys.prefix`.

**What:**
- Replaced the unconditional `py::scoped_interpreter guard(true, argc, argv);` with two branch-local declarations:
  - `#if PY_VERSION_HEX < 0x03080000`: keep `py::scoped_interpreter guard(true, argc, argv);` (pre-3.8 doesn't support `Py_InitializeFromConfig`-based init in pybind11 anyway, and `Py_SetProgramName` already covers the legacy path).
  - `#else`: use `py::scoped_interpreter guard(&config, argc, argv);` — pybind11's PyConfig-aware overload at [ext/pybind11/include/pybind11/embed.h:297](ext/pybind11/include/pybind11/embed.h#L297). That constructor delegates to `initialize_interpreter(PyConfig*, ...)` at [embed.h:139](ext/pybind11/include/pybind11/embed.h#L139), which calls `PyConfig_SetBytesArgv` then `Py_InitializeFromConfig(config)` then `PyConfig_Clear(config)`. Our `program_name` (and any future PyConfig fields we want to set) finally drive `sys.prefix` discovery.
- Both `guard` declarations are at function scope from the C++ point of view — `#if/#else` is a preprocessor split, not a C++ scope, so the rest of `main()` (importer install + `m5.main()`) sees `guard` regardless of which branch was compiled.
- No change to `detectVenvProgramName()` itself — it was correct in Round 1; the fix is consuming its output.

**Interactions / backward compatibility:**
- Pre-3.8 path is unchanged. The legacy `Py_SetProgramName` + `bool` scoped_interpreter still applies.
- 3.8+ path: when `VIRTUAL_ENV` is unset, `program_name` falls back to `argv[0]` (same value the existing code intended to set), so `sys.prefix` discovery now follows the path Python's PEP 405 logic produces from `argv[0]`. For a typical gem5.opt sitting outside any venv, that's `/usr` (or wherever the system Python lives) — same effective answer as before.
- `PyConfig_Clear` is called by pybind11's overload after `Py_InitializeFromConfig`. We must NOT call it ourselves. Removed any double-clear risk.
- The Round 1 warnings (stale `VIRTUAL_ENV`, half-baked venv) flow through the same `warn(...)` path; they were always being emitted, but `--outdir=...` redirects them to `m5out/simerr.txt` — verification will check that file rather than relying on stderr being visible in the foreground simout.

## 2026-04-30 — Round 3: Disable PyConfig argv parsing

**File(s):** [src/sim/main.cc](src/sim/main.cc).

**Why:** Round 2 swapped to pybind11's `scoped_interpreter(PyConfig*, ...)` overload, but the PyConfig version of pybind11's `initialize_interpreter` does NOT clear `config.parse_argv`. The default from `PyConfig_InitPythonConfig` is `parse_argv = 1`, so `Py_InitializeFromConfig` tried to interpret gem5's argv (e.g. `--outdir=...`) as CPython interpreter options and aborted with `unknown option --outdir=` followed by `Failed to init CPython` and a SIGABRT. The bool-overload of `initialize_interpreter` defends against exactly this at [ext/pybind11/include/pybind11/embed.h:198](ext/pybind11/include/pybind11/embed.h#L198) (`config.parse_argv = 0;` per pybind11 PR #4473). We need the same defense in our path because we're using the PyConfig overload directly.

**What:**
- Added `config.parse_argv = 0;` immediately after `PyConfig_InitPythonConfig(&config);` and before `PyConfig_SetBytesString` for `program_name`.
- Inline comment cites the pybind11 source line and PR for traceability.

**Interactions / backward compatibility:**
- This is the EXACT setting pybind11's bool overload uses, so behaviour for argv-parsing is byte-identical to what gem5 had before any of these rounds.
- `sys.argv` is still populated (via `PyConfig_SetBytesArgv` inside pybind11's PyConfig overload at [ext/pybind11/include/pybind11/embed.h:144](ext/pybind11/include/pybind11/embed.h#L144)) — only the *interpretation* of argv as Python flags is disabled.
- No change to `program_name` plumbing or to `detectVenvProgramName()`.
