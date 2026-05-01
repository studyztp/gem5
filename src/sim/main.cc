/*
 * Copyright (c) 2008 The Hewlett-Packard Development Company
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met: redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer;
 * redistributions in binary form must reproduce the above copyright
 * notice, this list of conditions and the following disclaimer in the
 * documentation and/or other materials provided with the distribution;
 * neither the name of the copyright holders nor the names of its
 * contributors may be used to endorse or promote products derived from
 * this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <Python.h>

#include <cstdlib>
#include <iostream>
#include <string>

// check if filesystem library is available — same conditional include
// pattern used in src/base/socket.cc.
#if defined(__cpp_lib_filesystem) || __has_include(<filesystem>)
    #include <filesystem>
#else
    #include <experimental/filesystem>
    namespace std {
        namespace filesystem = experimental::filesystem;
    }
#endif

#include "pybind11/embed.h"
#include "pybind11/pybind11.h"

#include "base/logging.hh"
#include "python/embedded.hh"
#include "sim/init_signals.hh"

using namespace gem5;

namespace py = pybind11;

namespace {

// Detect an active virtualenv at runtime and return the path of the
// venv's interpreter (suitable for PyConfig.program_name) so that
// Python's PEP 405 venv discovery picks up the venv's site-packages.
//
// Returns "" when no usable venv is active; the caller should fall
// back to argv[0] in that case to preserve the historical behaviour
// of gem5.opt embedding the build-time Python.
//
// Validation guards against the common stale-VIRTUAL_ENV failure mode
// (env var still exported after the venv was deleted, or pointing at a
// half-built directory).  We require BOTH:
//
//   1. <VIRTUAL_ENV>/pyvenv.cfg exists as a regular file — this is the
//      canonical PEP 405 marker for "this directory is a venv root".
//   2. <VIRTUAL_ENV>/bin/python exists — Python uses this path's
//      directory to locate pyvenv.cfg, then reads the `home = ...`
//      line to find the base interpreter's stdlib.
//
// On any validation failure we warn() and return "" so gem5 still
// starts cleanly with the compiled-in Python configuration instead of
// dying inside Py_Initialize() with a cryptic error.
//
// Setting GEM5_IGNORE_VENV=<anything non-empty> disables detection
// entirely — useful when the user has VIRTUAL_ENV exported globally
// but wants this particular gem5 invocation to use the system Python.
std::string
detectVenvProgramName()
{
    // Opt-out: any non-empty value disables detection.
    const char *ignore = std::getenv("GEM5_IGNORE_VENV");
    if (ignore != nullptr && ignore[0] != '\0') {
        return {};
    }

    const char *venv = std::getenv("VIRTUAL_ENV");
    if (venv == nullptr || venv[0] == '\0') {
        return {};
    }

    namespace fs = std::filesystem;
    fs::path root(venv);
    std::error_code ec;

    // PEP 405 marker.  Catches: "VIRTUAL_ENV points at a directory
    // that no longer exists / never was a venv".
    if (!fs::is_regular_file(root / "pyvenv.cfg", ec)) {
        warn("VIRTUAL_ENV=%s set but %s/pyvenv.cfg not found; falling "
             "back to compiled-in Python configuration. If this is "
             "intentional set GEM5_IGNORE_VENV=1 to suppress this "
             "warning.\n", venv, venv);
        return {};
    }

    // The path Python will treat as the "interpreter executable" for
    // sys.prefix discovery.  Catches: "venv directory exists but is
    // missing bin/python" (unlikely in practice, but cheap to guard).
    fs::path bin_python = root / "bin" / "python";
    if (!fs::exists(bin_python, ec)) {
        warn("VIRTUAL_ENV=%s has pyvenv.cfg but %s/bin/python is "
             "missing; falling back to compiled-in Python "
             "configuration.\n", venv, venv);
        return {};
    }

    return bin_python.string();
}

} // anonymous namespace

// main() is now pretty stripped down and just sets up python and then
// calls EmbeddedPython::initAll which loads the various embedded python
// modules into the python environment and then starts things running by
// running python's m5.main().
int
main(int argc, char **argv)
{
    // Initialize gem5 special signal handling.
    initSignals();

#if PY_VERSION_HEX < 0x03080000
    // Convert argv[0] to a wchar_t string, using python's locale and cleanup
    // functions.
    std::unique_ptr<wchar_t[], decltype(&PyMem_RawFree)> program(
        Py_DecodeLocale(argv[0], nullptr),
        &PyMem_RawFree);

    // This can help python find libraries at run time relative to this binary.
    // It's probably not necessary, but is mostly harmless and might be useful.
    Py_SetProgramName(program.get());

    // Older Python: pybind11 initializes the interpreter with its own
    // default config.  The PyConfig-based venv-aware path below is not
    // available before 3.8.
    py::scoped_interpreter guard(true, argc, argv);
#else
    // Preinitialize Python for Python 3.8+
    // This ensures that the locale configuration takes effect
    PyStatus status;

    PyConfig config;
    PyConfig_InitPythonConfig(&config);

    // Disable Python's argv parser.  PyConfig_InitPythonConfig defaults
    // to parse_argv=1, which makes Py_InitializeFromConfig try to
    // interpret argv entries as CPython interpreter options (-c, -m,
    // unknown flags, …).  gem5's argv contains gem5-specific flags
    // like --outdir that CPython doesn't recognise, so leaving this
    // at the default aborts startup with "unknown option ...".
    // pybind11's bool-overload of initialize_interpreter sets this to
    // 0 internally (see ext/pybind11/include/pybind11/embed.h:198 and
    // pybind11 PR #4473) — we mirror that here for the PyConfig
    // overload we've now switched to.
    config.parse_argv = 0;

    // Honour an active virtualenv if one is configured.  When
    // VIRTUAL_ENV is set and validates as a real PEP 405 venv,
    // detectVenvProgramName() returns "<venv>/bin/python" and Python's
    // standard venv discovery (reads pyvenv.cfg, redirects sys.prefix,
    // adds venv site-packages to sys.path) fires.  When detection
    // returns "" (no venv, or stale/invalid VIRTUAL_ENV — already
    // warn()'d), we fall back to argv[0] for the historical behaviour.
    std::string venv_program = detectVenvProgramName();
    const char *program_name =
        venv_program.empty() ? argv[0] : venv_program.c_str();

    /* Set the program name. Implicitly preinitialize Python. */
    status = PyConfig_SetBytesString(&config, &config.program_name,
                                     program_name);
    if (PyStatus_Exception(status)) {
        PyConfig_Clear(&config);
        Py_ExitStatusException(status);
        return 1;
    }

    // Pass our PyConfig (populated above with program_name pointing at
    // the venv interpreter when applicable) to pybind11's PyConfig-aware
    // scoped_interpreter overload at ext/pybind11/include/pybind11/embed.h.
    // That overload calls Py_InitializeFromConfig() and clears the config
    // for us, so the program_name we set actually drives sys.prefix
    // discovery — the bool overload silently ignored our config.
    py::scoped_interpreter guard(&config, argc, argv);
#endif

    auto importer = py::module_::import("importer");
    importer.attr("install")();

    try {
        py::module_::import("m5").attr("main")();
    } catch (py::error_already_set &e) {
        if (e.matches(PyExc_SystemExit))
            return e.value().attr("code").cast<int>();

        std::cerr << e.what();
        return 1;
    }

    return 0;
}
