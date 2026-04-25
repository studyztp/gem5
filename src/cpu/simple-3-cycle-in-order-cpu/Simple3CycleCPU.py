# Copyright (c) 2026 Zhantong Qiu, University of California, Davis
# and Cornell University
# All rights reserved.
#
# Redistribution and use in source and binary forms, with or without
# modification, are permitted provided that the following conditions are
# met: redistributions of source code must retain the above copyright
# notice, this list of conditions and the following disclaimer;
# redistributions in binary form must reproduce the above copyright
# notice, this list of conditions and the following disclaimer in the
# documentation and/or other materials provided with the distribution;
# neither the name of the copyright holders nor the names of its
# contributors may be used to endorse or promote products derived from
# this software without specific prior written permission.
#
# THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
# "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
# LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
# A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
# OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
# SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
# LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
# DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
# THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
# (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
# OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

from m5.objects.BaseCPU import BaseCPU
from m5.params import *


class Simple3CycleCPU(BaseCPU):
    """Simple 3-stage in-order CPU (F / D / E).

    Timing-model port of the Cortex-M4 Python prototype under
    prototype/stm32g4_pipeline_model.py.  ISA-agnostic: the CPU
    source files contain no arch/<isa>/ includes and rely on gem5's
    generic InstDecoder and StaticInst interfaces.

    Flash / AHB timing is NOT modelled inside this CPU.  Connect
    icache_port to a PipelinedSimpleMemory to get the 64-bit read
    latch, wait-state miss, and AHB retraction semantics the
    Cortex-M4 + STM32G4 flash expects.
    """

    type = "Simple3CycleCPU"
    cxx_header = "cpu/simple-3-cycle-in-order-cpu/simple_3cycle_cpu.hh"
    cxx_class = "gem5::simple3::Simple3CycleCPU"

    @classmethod
    def memory_mode(cls):
        return "timing"

    # Pipeline refill cycles on a redirected / mispredicted branch.
    # Cortex-M4 TRM Table 3-1 footnote c: "P = 1..3 depending on
    # alignment and whether the target address is speculated early".
    # Worst case is 3.
    mispredict_flush_cycles = Param.Cycles(
        3, "Pipeline refill cycles after a mispredict or D-stage redirect"
    )

    # Cycles between an E→D-forwarded branch resolution and the first
    # HCLK at which F can issue the redirect-target fetch.  Empirical
    # tuning: silicon's 10 HCLK/iter on bench-branch implies ~2
    # additional HCLK per taken redirect compared to a model that
    # issues F the same cycle the branch resolves.  The Cortex-M4 TRM
    # does not decompose this — see m-class-skill cite chain in
    # PROGRESS.md.  Plausible architectural sources: (a) next-HCLK
    # forwarding (write at E in cycle T, read at D in cycle T+1),
    # (b) 1-HCLK AHB address-phase setup before the new ICode HTRANS
    # NONSEQ can be driven.
    taken_branch_redirect_delay = Param.Cycles(
        2,
        "HCLK between an E→D-resolved taken branch and the first HCLK "
        "at which F can issue the target fetch.  Models the AHB bus "
        "address-phase setup after a redirect.  The TAKEN_BRANCH_HCLK "
        "bundle (silicon pays ~10 HCLK regardless of buffer state) is "
        "modelled separately by marking the first post-redirect fetch "
        "UNCACHEABLE so PipelinedSimpleMemory bypasses its buffer-hit "
        "shortcut and pays full WS latency.",
    )

    # PFU FIFO depth in 32-bit fetch words.  Cortex-M4 uses a 3-word
    # prefetch queue.
    pfu_fifo_words = Param.Unsigned(
        3, "PFU prefetch FIFO depth, measured in 32-bit fetch words"
    )

    # Halt PC: when architectural PC reaches this value and the
    # pipeline is drained, the CPU stops ticking.  Set to the
    # address of your test's exit / m5 exit instruction.
    halt_addr = Param.Addr(
        0, "Architectural PC at which the CPU halts (0 = disabled)"
    )

    # Phase-B synthetic test stream: when nonzero, the CPU
    # pre-populates its PFU FIFO with this many fetch words filled
    # with `phase_b_word_value` at sequential addresses starting at
    # `phase_b_start_pc`.  Used only by the Phase-B smoke test;
    # leave at 0 for normal use.
    phase_b_num_words = Param.Unsigned(
        0,
        "If nonzero, hand-populate PFU FIFO with this many words "
        "(Phase-B test only)",
    )
    phase_b_word_value = Param.UInt32(
        0, "Value of each pre-populated fetch word " "(Phase-B test only)"
    )
    phase_b_start_pc = Param.Addr(
        0,
        "Address of the first pre-populated fetch word " "(Phase-B test only)",
    )

    # Phase-C synthetic test stream: when nonzero, the CPU writes
    # `phase_c_num_words` copies of `phase_c_word_value` into system
    # memory at `phase_c_start_pc` (via system port functional
    # access) during startup().  Unlike Phase-B, the FIFO is NOT
    # pre-populated — fetches go through icache_port.  Used by the
    # Phase-C smoke test; leave at 0 for normal use.
    phase_c_num_words = Param.Unsigned(
        0,
        "If nonzero, write this many words into memory at startup "
        "(Phase-C test only)",
    )
    phase_c_word_value = Param.UInt32(
        0, "Value of each written word (Phase-C test only)"
    )
    phase_c_start_pc = Param.Addr(
        0, "Memory address at which to write the words " "(Phase-C test only)"
    )

    # Phase-D synthetic program: same memory-init mechanism as
    # Phase C, but each entry of the vector is its own 32-bit word
    # (so a hand-coded Thumb-2 kernel like
    # [movs|subs, bne|nop, nop|nop, ...]) lands at consecutive
    # word addresses starting at `phase_d_start_pc`.  Used by the
    # Phase-D smoke test which exercises the real ALU and the
    # mispredict path.
    phase_d_words = VectorParam.UInt32(
        [],
        "Sequence of 32-bit fetch words to write into memory at "
        "startup (Phase-D test only).  Empty vector disables.",
    )
    phase_d_start_pc = Param.Addr(
        0,
        "Memory address of the first phase_d_words entry — must be "
        "word-aligned (Phase-D test only)",
    )

    # Optional override for the architectural entry PC.  When 0 the
    # CPU uses phase_d_start_pc as the entry PC.  Set this when the
    # kernel's first instruction lives mid-word (e.g. a 16-bit Thumb
    # at addr+2 of an 8-byte-aligned line) so memory writes can stay
    # word-aligned while the thread starts at the right halfword.
    phase_d_entry_pc = Param.Addr(
        0,
        "If non-zero, override the architectural start PC for the "
        "thread; otherwise phase_d_start_pc is used (Phase-D only)",
    )
