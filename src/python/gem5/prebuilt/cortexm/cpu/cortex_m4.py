# Copyright (c) 2026 University of California, Davis and Cornell University
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

"""
Cortex-M4 timing-tuned MinorCPU for STM32G474RE.

Pipeline and FU latencies from DDI0439D (Cortex-M4 TRM):
  - 3-stage pipeline (Fetch, Decode, Execute), single-issue, in-order
  - Single-cycle 32x32 multiplier
  - Data-dependent division (2-12 cycles)
  - VFPv4 single-precision FPU
  - No architectural branch prediction hardware

Follows the U74CPU pattern from prebuilt/riscvmatched/riscvmatched_core.py.
"""

from m5.objects.ArmMCPU import ArmMMinorCPU
from m5.objects.BaseMinorCPU import (
    MinorFU,
    MinorFUPool,
    MinorFUTiming,
    minorMakeOpClassSet,
)
from m5.objects.BranchPredictor import (
    BranchPredictor,
    LocalBP,
    ReturnAddrStack,
    SimpleBTB,
)
from m5.params import NULL

# ---------------------------------------------------------------------------
# Functional Units — DDI0439D Table 3-1 (pages 3-4 to 3-8)
# ---------------------------------------------------------------------------


class M4IntAluFU(MinorFU):
    """ADD, SUB, MOV, AND, ORR, EOR, CMP, CMN, TST, TEQ, CLZ, REV, etc."""

    opClasses = minorMakeOpClassSet(["IntAlu"])
    opLat = 1
    issueLat = 1
    timings = [MinorFUTiming(description="M4Int", srcRegsRelativeLats=[1])]


class M4IntMulFU(MinorFU):
    """MUL, MLA, MLS, SMULL, UMULL, SMLAL, UMLAL — single-cycle 32x32."""

    opClasses = minorMakeOpClassSet(["IntMult"])
    opLat = 1
    issueLat = 1
    timings = [MinorFUTiming(description="M4Mul", srcRegsRelativeLats=[0])]


class M4IntDivFU(MinorFU):
    """SDIV, UDIV — 2-12 cycles data-dependent; 7 = representative average.
    Non-pipelined (blocks issue for the full duration)."""

    opClasses = minorMakeOpClassSet(["IntDiv"])
    opLat = 7
    issueLat = 7


class M4FloatFU(MinorFU):
    """VFPv4 single-precision + DSP SIMD.  VADD/VSUB/VMUL/VCMP/VCVT = 1 cy.
    VMLA/VFMA=3cy and VDIV/VSQRT=14cy are underestimated at opLat=1;
    would need MinorFUTiming rules for per-instruction latency."""

    opClasses = minorMakeOpClassSet(
        [
            "FloatAdd",
            "FloatCmp",
            "FloatCvt",
            "FloatMisc",
            "FloatMult",
            "FloatMultAcc",
            "FloatDiv",
            "FloatSqrt",
            "SimdAdd",
            "SimdAlu",
            "SimdCmp",
            "SimdMisc",
            "SimdMult",
            "SimdMultAcc",
            "SimdShift",
            "SimdShiftAcc",
        ]
    )
    opLat = 1
    issueLat = 1
    timings = [MinorFUTiming(description="M4Float", srcRegsRelativeLats=[1])]


class M4MemFU(MinorFU):
    """Load/Store unit.  1 cycle for address calculation; actual memory
    latency comes from the memory system (bus + SimpleMemory/cache)."""

    opClasses = minorMakeOpClassSet(
        ["MemRead", "MemWrite", "FloatMemRead", "FloatMemWrite"]
    )
    opLat = 1
    issueLat = 1
    timings = [
        MinorFUTiming(
            description="M4Mem", srcRegsRelativeLats=[1], extraAssumedLat=2
        )
    ]


class M4MiscFU(MinorFU):
    """DMB, DSB, ISB, NOP, BKPT, system instructions."""

    opClasses = minorMakeOpClassSet(["InstPrefetch", "System"])
    opLat = 1
    issueLat = 1


class CortexM4FUPool(MinorFUPool):
    funcUnits = [
        M4IntAluFU(),
        M4IntMulFU(),
        M4IntDivFU(),
        M4FloatFU(),
        M4MemFU(),
        M4MiscFU(),
    ]


# ---------------------------------------------------------------------------
# Branch Predictor — minimal, M4 has no architectural BP [DDI0439D §2.1]
# ---------------------------------------------------------------------------


class CortexM4BP(BranchPredictor):
    """Minimal predictor: M4 has no documented branch prediction hardware.
    We use a tiny LocalBP to capture tight-loop patterns without being
    unrealistically accurate."""

    instShiftAmt = 1  # Thumb: 2-byte (half-word) aligned
    conditionalBranchPred = LocalBP(localPredictorSize=64, localCtrBits=2)
    btb = SimpleBTB(numEntries=16, tagBits=16, instShiftAmt=1)
    ras = ReturnAddrStack(numEntries=4)
    indirectBranchPred = NULL


# ---------------------------------------------------------------------------
# CortexM4CPU — ArmMMinorCPU with M4-tuned pipeline
# ---------------------------------------------------------------------------


class CortexM4CPU(ArmMMinorCPU):
    """
    Timing-tuned MinorCPU modelling the Cortex-M4 3-stage pipeline.

    The real M4 has 3 stages (Fetch, Decode, Execute) [DDI0439D §2.1].
    MinorCPU is hardcoded at 4 stages (Fetch1, Fetch2, Decode, Execute).
    We collapse Fetch1+Fetch2 by setting fetch1ToFetch2BackwardDelay=0
    (same-cycle feedback), effectively making them one logical stage.

    All widths are 1 (single-issue, single-decode, single-commit).
    """

    threadPolicy = "SingleThreaded"

    # -- Fetch stage (collapsed Fetch1 + Fetch2) --
    # The real M4 ICode bus is 32-bit [DDI0439D §2.2.1], so fetch width
    # is 4 bytes.  The ART cache handles the 4B→8B translation between
    # the CPU fetch size and the 64-bit flash read width internally.
    fetch1FetchLimit = 1
    fetch1LineSnapWidth = 4  # 32-bit ICode bus [DDI0439D §2.2.1]
    fetch1LineWidth = 4  # 32-bit ICode bus [DDI0439D §2.2.1]
    fetch1ToFetch2ForwardDelay = 1  # minimum (cannot be 0)
    fetch1ToFetch2BackwardDelay = 0  # same-cycle: collapses F1+F2

    fetch2InputBufferSize = 1
    fetch2ToDecodeForwardDelay = 1
    fetch2CycleInput = True

    # -- Decode stage --
    decodeInputBufferSize = 1
    decodeToExecuteForwardDelay = 1
    decodeInputWidth = 1  # SINGLE-ISSUE
    decodeCycleInput = True

    # -- Execute stage --
    executeInputWidth = 1  # SINGLE-ISSUE
    executeCycleInput = True
    executeIssueLimit = 1  # SINGLE-ISSUE
    executeCommitLimit = 1  # SINGLE-ISSUE
    executeMemoryIssueLimit = 1
    executeMemoryCommitLimit = 1
    executeInputBufferSize = 3
    executeMaxAccessesInMemory = 1  # no memory-level parallelism
    executeLSQMaxStoreBufferStoresPerCycle = 1
    executeLSQRequestsQueueSize = 1
    executeLSQTransfersQueueSize = 1
    executeLSQStoreBufferSize = 1  # M4 has very small store buffer
    executeBranchDelay = 1
    executeMemoryWidth = 8  # max LSQ transfer width (LDRD/STRD = 8 bytes)
    executeSetTraceTimeOnCommit = True
    executeSetTraceTimeOnIssue = False
    executeAllowEarlyMemoryIssue = True
    enableIdling = True

    # -- FU pool and branch predictor --
    executeFuncUnits = CortexM4FUPool()
    branchPred = CortexM4BP()
