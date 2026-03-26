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
  - VFPv4 single-precision FPU:
      VADD/VSUB/VMUL/VCMP = 1 cy  [Table 7-1]
      VMLA/VFMA = 3 cy            [Table 7-1]
      VDIV/VSQRT = 14 cy          [Table 7-1]
  - Limited address speculation (reduces branch P from 3 to 1)
  - STR effective = 1 cy with write buffer [§3.3.2 p.3-11]
  - LDR pipelining: back-to-back LDR = 1 cy each [§3.3.2 p.3-12]

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
    """VFPv4 single-precision + DSP SIMD — single-cycle operations.
    VADD/VSUB/VMUL/VCMP/VCVT = 1 cy [DDI0439D Table 7-1]."""

    opClasses = minorMakeOpClassSet(
        [
            "FloatAdd",
            "FloatCmp",
            "FloatCvt",
            "FloatMisc",
            "FloatMult",
            "SimdAdd",
            "SimdAlu",
            "SimdCmp",
            "SimdMisc",
            "SimdMult",
            "SimdShift",
            "SimdShiftAcc",
        ]
    )
    opLat = 1
    issueLat = 1
    timings = [MinorFUTiming(description="M4Float", srcRegsRelativeLats=[1])]


class M4FloatMacFU(MinorFU):
    """VMLA/VFMA/VFMS/VNMLA/VNMLS — 3-cycle multiply-accumulate.
    Non-pipelined [DDI0439D Table 7-1]."""

    opClasses = minorMakeOpClassSet(["FloatMultAcc", "SimdMultAcc"])
    opLat = 3
    issueLat = 3
    timings = [MinorFUTiming(description="M4FMac", srcRegsRelativeLats=[2])]


class M4FloatDivFU(MinorFU):
    """VDIV.F32 = 14 cy, VSQRT.F32 = 14 cy — non-pipelined.
    [DDI0439D Table 7-1, pages 7-4/7-5]."""

    opClasses = minorMakeOpClassSet(["FloatDiv", "FloatSqrt"])
    opLat = 14
    issueLat = 14


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
        M4FloatMacFU(),
        M4FloatDivFU(),
        M4MemFU(),
        M4MiscFU(),
    ]


# ---------------------------------------------------------------------------
# Branch Predictor — M4 has limited speculative address resolution
# [DDI0439D §3.3.1, P definition: "whether the processor manages to
# speculate the address early"].  Not a full branch predictor, but the
# TRM documents that P can be reduced from 3 to 1 via speculation.
# We use a small LocalBP to approximate this limited speculation.
# ---------------------------------------------------------------------------


class CortexM4BP(BranchPredictor):
    """Small predictor approximating the M4's limited address speculation.
    The Cortex-M4 can 'speculate the address early' [DDI0439D p.3-4],
    reducing P from worst-case 3 to best-case 1.  A tiny LocalBP captures
    tight-loop patterns without being unrealistically accurate."""

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
    #
    # fetch1FetchLimit=1: only one fetch in flight at a time.
    # The ART cache can only handle one outstanding cache lookup
    # (cachePktEntry is a single slot).  fetchLimit=2 would require
    # the ART to queue or block requests, which conflicts with
    # BaseCache's own setBlocked/clearBlocked mechanism.
    # The 1-cycle pipeline overhead per fetch is a known gem5 limitation
    # (real M4 achieves 0-WS fetch from ART buffers).
    fetch1FetchLimit = 1
    fetch1LineSnapWidth = 4  # 32-bit ICode bus [DDI0439D §2.2.1]
    fetch1LineWidth = 4  # 32-bit ICode bus [DDI0439D §2.2.1]
    fetch1ToFetch2ForwardDelay = 1  # minimum (cannot be 0)
    fetch1ToFetch2BackwardDelay = 0  # same-cycle: collapses F1+F2

    # fetch2InputBufferSize=2 lets Fetch2 hold 2 lines so that Fetch1
    # can forward the next line while Fetch2 is still draining the
    # current one.  Without this, fetchLimit=2 would stall on back-
    # pressure from Fetch2's single-entry buffer.
    fetch2InputBufferSize = 2
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

    # -- Memory / LSQ parameters --
    # The Cortex-M4 has a write buffer that decouples stores from the
    # pipeline: "STR Rx,[Ry,#imm] is always one cycle" [DDI0439D §3.3.2
    # p.3-11].  We model this with a 3-entry store buffer and allow
    # 2 concurrent memory accesses so that stores can drain in the
    # background while the next load/store issues.
    #
    # Also: PUSH/POP decompose into N micro-ops in gem5.  With only 1
    # concurrent access, each micro-op is fully serialized (~6 cy each).
    # Allowing 2 concurrent accesses lets the pipeline overlap address
    # generation with data transfer, reducing PUSH/POP from ~5x to ~2-3x
    # overcount.  (Full 1+N accuracy requires C++ burst-transfer support.)
    executeMaxAccessesInMemory = 2
    executeLSQMaxStoreBufferStoresPerCycle = 2
    executeLSQRequestsQueueSize = 1
    executeLSQTransfersQueueSize = 2
    executeLSQStoreBufferSize = 3
    executeBranchDelay = 1
    executeMemoryWidth = 8  # max LSQ transfer width (LDRD/STRD = 8 bytes)
    executeSetTraceTimeOnCommit = True
    executeSetTraceTimeOnIssue = False
    executeAllowEarlyMemoryIssue = True
    enableIdling = True

    # -- FU pool and branch predictor --
    executeFuncUnits = CortexM4FUPool()
    branchPred = CortexM4BP()
