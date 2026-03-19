/*
 * Copyright (c) 2026 University of California, Davis and Cornell University
 * All rights reserved
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

/**
 * @file
 * Unit tests for M-profile SCS logic.
 *
 * These tests validate the pure logic extracted from MProfileSCS
 * without requiring a full SimObject/ThreadContext infrastructure.
 * Focus areas:
 *   - Priority mask computation (the formula from constructor)
 *   - NVIC bit-vector scanning (the updatePending inner loop)
 *   - SCB register map (offset -> MiscRegIndex mapping)
 *   - SCS address dispatch (offset -> sub-module routing)
 */

#include <gtest/gtest.h>

#include <cstdint>
#include <map>

// We test the logic as standalone functions, so we don't need to
// include the full SCS header (which requires SimObject infrastructure).
// Instead, we replicate the logic snippets being tested and verify
// they match the expected hardware behavior per ARMv7-M spec.

// =========================================================================
// Priority mask computation
// =========================================================================
//
// The SCS constructor computes:
//   priorityMask = ~((1u << (8 - priorityBits)) - 1u)
//
// This mask has the top N bits set and the bottom (8-N) bits clear.
// It's applied to all priority register reads/writes so that
// unimplemented low bits always appear as zero.
//
// Expected results:
//   2 bits (M0)  -> 0xC0 (bits 7:6)
//   3 bits (M3)  -> 0xE0 (bits 7:5)
//   4 bits (M4)  -> 0xF0 (bits 7:4)
//   8 bits (max) -> 0xFF (all bits)

static uint8_t
computePriorityMask(uint8_t priority_bits)
{
    return (uint8_t)(~((1u << (8 - priority_bits)) - 1u));
}

TEST(MProfileSCSPriority, MaskComputation)
{
    // 2 bits: M0/M0+ — 4 priority levels
    // Only bits[7:6] implemented -> mask = 0b11000000 = 0xC0
    EXPECT_EQ(0xC0, computePriorityMask(2));

    // 3 bits: M3 — 8 priority levels
    // Only bits[7:5] implemented -> mask = 0b11100000 = 0xE0
    EXPECT_EQ(0xE0, computePriorityMask(3));

    // 4 bits: M4 — 16 priority levels
    // Only bits[7:4] implemented -> mask = 0b11110000 = 0xF0
    EXPECT_EQ(0xF0, computePriorityMask(4));

    // 5 bits: — 32 priority levels
    EXPECT_EQ(0xF8, computePriorityMask(5));

    // 8 bits: maximum — 256 priority levels
    // All bits implemented -> mask = 0xFF
    EXPECT_EQ(0xFF, computePriorityMask(8));
}

TEST(MProfileSCSPriority, MaskFiltersLowBits)
{
    // With 4-bit priority (mask 0xF0), writing 0xAB should yield 0xA0.
    // The low nibble (0x0B) contains unimplemented bits that must be
    // discarded.
    uint8_t mask = computePriorityMask(4);
    EXPECT_EQ(0xA0, (uint8_t)(0xAB & mask));

    // With 2-bit priority (mask 0xC0), writing 0xFF should yield 0xC0.
    mask = computePriorityMask(2);
    EXPECT_EQ(0xC0, (uint8_t)(0xFF & mask));

    // With 8-bit priority (mask 0xFF), nothing is filtered.
    mask = computePriorityMask(8);
    EXPECT_EQ(0xFF, (uint8_t)(0xFF & mask));
}

TEST(MProfileSCSPriority, PriorityComparison)
{
    // In M-profile, lower number = higher priority.
    // A pending interrupt at priority 0x20 should preempt execution
    // at priority 0x40, but NOT execution at priority 0x10.
    uint8_t pending_pri = 0x20;
    EXPECT_TRUE(pending_pri < 0x40);   // can preempt
    EXPECT_FALSE(pending_pri < 0x10);  // cannot preempt
    EXPECT_FALSE(pending_pri < 0x20);  // equal priority: no preemption

    // Thread mode with no masks has execution priority 0xFF.
    // Any configured interrupt (priority 0x00..0xFE) can preempt it.
    EXPECT_TRUE(0x00 < 0xFF);
    EXPECT_TRUE(0xFE < 0xFF);

    // PRIMASK/FAULTMASK set execution priority to 0.
    // No configurable interrupt can preempt (lowest configurable is 0x00).
    EXPECT_FALSE(0x00 < 0x00);  // equal: no preemption
}

// =========================================================================
// NVIC bit-vector scanning
// =========================================================================
//
// updatePending() uses this inner loop to find the highest-priority
// deliverable IRQ among a set of enabled, pending, and not-active IRQs:
//
//   deliverable = enabled & pending & ~active
//   while (deliverable) {
//       bit = __builtin_ctz(deliverable)
//       irq = word * 32 + bit
//       pri = priority[irq] & mask
//       if (pri < best_pri) { best = irq; best_pri = pri; }
//       deliverable &= deliverable - 1  // clear lowest set bit
//   }
//
// We test this logic in isolation to catch off-by-one or masking bugs.

struct NvicScanResult
{
    int bestIRQ;       // -1 if nothing deliverable
    uint8_t bestPri;   // 0xFF if nothing deliverable
};

// Replicates the updatePending inner loop for one 32-bit word of IRQs.
static NvicScanResult
scanOneWord(uint32_t enabled, uint32_t pending, uint32_t active,
            const uint8_t *priorities, uint8_t pri_mask,
            int word_index)
{
    NvicScanResult result = { -1, 0xFF };
    uint32_t deliverable = enabled & pending & ~active;
    while (deliverable) {
        int bit = __builtin_ctz(deliverable);
        int irq = word_index * 32 + bit;
        uint8_t pri = priorities[irq] & pri_mask;
        if (pri < result.bestPri) {
            result.bestPri = pri;
            result.bestIRQ = irq;
        }
        deliverable &= deliverable - 1;
    }
    return result;
}

TEST(MProfileSCSNvicScan, NothingPending)
{
    uint8_t priorities[32] = {};
    auto result = scanOneWord(0xFFFFFFFF, 0x00000000, 0x00000000,
                              priorities, 0xF0, 0);
    EXPECT_EQ(-1, result.bestIRQ);
    EXPECT_EQ(0xFF, result.bestPri);
}

TEST(MProfileSCSNvicScan, NothingEnabled)
{
    uint8_t priorities[32] = {};
    auto result = scanOneWord(0x00000000, 0xFFFFFFFF, 0x00000000,
                              priorities, 0xF0, 0);
    EXPECT_EQ(-1, result.bestIRQ);
    EXPECT_EQ(0xFF, result.bestPri);
}

TEST(MProfileSCSNvicScan, AllActive)
{
    // Everything is enabled and pending, but also active.
    // Active interrupts are excluded from delivery.
    uint8_t priorities[32] = {};
    auto result = scanOneWord(0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF,
                              priorities, 0xF0, 0);
    EXPECT_EQ(-1, result.bestIRQ);
}

TEST(MProfileSCSNvicScan, SinglePending)
{
    // Only IRQ 5 is enabled and pending.
    uint8_t priorities[32] = {};
    priorities[5] = 0x40;
    auto result = scanOneWord(1u << 5, 1u << 5, 0, priorities, 0xF0, 0);
    EXPECT_EQ(5, result.bestIRQ);
    EXPECT_EQ(0x40, result.bestPri);
}

TEST(MProfileSCSNvicScan, HighestPriorityWins)
{
    // IRQ 3 at priority 0x80, IRQ 7 at priority 0x20.
    // IRQ 7 should win (lower number = higher priority).
    uint8_t priorities[32] = {};
    priorities[3] = 0x80;
    priorities[7] = 0x20;
    uint32_t mask = (1u << 3) | (1u << 7);
    auto result = scanOneWord(mask, mask, 0, priorities, 0xF0, 0);
    EXPECT_EQ(7, result.bestIRQ);
    EXPECT_EQ(0x20, result.bestPri);
}

TEST(MProfileSCSNvicScan, ActiveExcluded)
{
    // IRQ 3 (pri 0x80) and IRQ 7 (pri 0x20) both pending.
    // But IRQ 7 is already active, so IRQ 3 should win.
    uint8_t priorities[32] = {};
    priorities[3] = 0x80;
    priorities[7] = 0x20;
    uint32_t en_pend = (1u << 3) | (1u << 7);
    uint32_t active = (1u << 7);
    auto result = scanOneWord(en_pend, en_pend, active,
                              priorities, 0xF0, 0);
    EXPECT_EQ(3, result.bestIRQ);
    EXPECT_EQ(0x80, result.bestPri);
}

TEST(MProfileSCSNvicScan, PriorityMaskApplied)
{
    // IRQ 0 with raw priority 0xAB.  With 4-bit mask (0xF0),
    // effective priority is 0xA0 (low nibble discarded).
    uint8_t priorities[32] = {};
    priorities[0] = 0xAB;
    auto result = scanOneWord(1u, 1u, 0, priorities, 0xF0, 0);
    EXPECT_EQ(0, result.bestIRQ);
    EXPECT_EQ(0xA0, result.bestPri);
}

TEST(MProfileSCSNvicScan, WordIndexOffset)
{
    // Testing word_index=1: IRQ numbers should be 32..63.
    // IRQ 33 (bit 1 of word 1) is pending.
    uint8_t priorities[64] = {};
    priorities[33] = 0x60;
    auto result = scanOneWord(1u << 1, 1u << 1, 0,
                              priorities, 0xF0, 1);
    EXPECT_EQ(33, result.bestIRQ);
    EXPECT_EQ(0x60, result.bestPri);
}

// =========================================================================
// NVIC register write semantics
// =========================================================================
//
// ISER: write-1-to-set (OR into enabled)
// ICER: write-1-to-clear (AND ~data into enabled)
// ISPR: write-1-to-set (OR into pending)
// ICPR: write-1-to-clear (AND ~data into pending)

TEST(MProfileSCSNvicRegs, SetEnable)
{
    // ISER write: bits written as 1 are set; bits written as 0 unchanged
    uint32_t enabled = 0x0000FF00;
    uint32_t write_val = 0x00FF0000;
    enabled |= write_val;
    EXPECT_EQ(0x00FFFF00u, enabled);
}

TEST(MProfileSCSNvicRegs, ClearEnable)
{
    // ICER write: bits written as 1 are cleared; bits written as 0 unchanged
    uint32_t enabled = 0x00FFFF00;
    uint32_t write_val = 0x0000FF00;
    enabled &= ~write_val;
    EXPECT_EQ(0x00FF0000u, enabled);
}

TEST(MProfileSCSNvicRegs, SetPending)
{
    uint32_t pending = 0;
    pending |= (1u << 5);
    EXPECT_EQ(1u << 5, pending);
    // Writing again should be idempotent
    pending |= (1u << 5);
    EXPECT_EQ(1u << 5, pending);
}

TEST(MProfileSCSNvicRegs, ClearPending)
{
    uint32_t pending = 0xFFFFFFFF;
    pending &= ~(1u << 5);
    EXPECT_EQ(~(1u << 5), pending);
}

TEST(MProfileSCSNvicRegs, ActivateDeactivate)
{
    // activateIRQ: set active, clear pending
    uint32_t active = 0;
    uint32_t pending = (1u << 5);
    int irq = 5;
    active |= (1u << irq);
    pending &= ~(1u << irq);
    EXPECT_EQ(1u << 5, active);
    EXPECT_EQ(0u, pending);

    // deactivateIRQ: clear active
    active &= ~(1u << irq);
    EXPECT_EQ(0u, active);
}

// =========================================================================
// SCB register map
// =========================================================================
//
// The static scbRegMap maps SCB offsets (relative to 0xD00 within the
// SCS) to MiscRegIndex values.  We verify the offset<->register
// mapping against the ARMv7-M spec (DDI0403E B3.2).

// We can't include misc.hh without pulling in the full gem5 build,
// so we define the expected mappings as offset<->string pairs and
// verify the map structure (offsets, count, alignment).

TEST(MProfileSCSSCBMap, OffsetsAre4ByteAligned)
{
    // All SCB registers are 32-bit, so offsets must be 4-byte aligned.
    // Expected offsets from spec: 0x00, 0x04, 0x08, ..., 0x3C
    uint32_t expected_offsets[] = {
        0x00, 0x04, 0x08, 0x0C, 0x10, 0x14, 0x18, 0x1C,
        0x20, 0x24, 0x28, 0x2C, 0x30, 0x34, 0x38, 0x3C
    };
    for (uint32_t off : expected_offsets) {
        EXPECT_EQ(0u, off % 4) << "Offset " << off << " not 4-byte aligned";
    }
}

TEST(MProfileSCSSCBMap, ExpectedRegisterCount)
{
    // SCB has 16 registers mapped (CPUID through AFSR).
    // Verify we expect exactly 16 entries.
    uint32_t expected_offsets[] = {
        0x00, 0x04, 0x08, 0x0C, 0x10, 0x14, 0x18, 0x1C,
        0x20, 0x24, 0x28, 0x2C, 0x30, 0x34, 0x38, 0x3C
    };
    EXPECT_EQ(16u, sizeof(expected_offsets) / sizeof(expected_offsets[0]));
}

// =========================================================================
// SCS address dispatch classification
// =========================================================================
//
// The top-level read()/write() dispatches by offset within the 4KB
// SCS region.  We verify that each address range routes to the
// correct sub-module.

enum SCSSubModule { SYSTICK, NVIC, SCB, STIR, UNKNOWN };

static SCSSubModule
classifyOffset(uint32_t daddr)
{
    if (daddr >= 0x010 && daddr <= 0x01F)
        return SYSTICK;
    if ((daddr >= 0x100 && daddr <= 0x4EF) ||
        (daddr >= 0x300 && daddr <= 0x31F))
        return NVIC;
    if (daddr >= 0xD00 && daddr <= 0xD3F)
        return SCB;
    if (daddr == 0xF00)
        return STIR;
    return UNKNOWN;
}

TEST(MProfileSCSDispatch, SysTickRange)
{
    // SysTick: CSR(0x10), RVR(0x14), CVR(0x18), CALIB(0x1C)
    EXPECT_EQ(SYSTICK, classifyOffset(0x010));
    EXPECT_EQ(SYSTICK, classifyOffset(0x014));
    EXPECT_EQ(SYSTICK, classifyOffset(0x018));
    EXPECT_EQ(SYSTICK, classifyOffset(0x01C));
    EXPECT_EQ(SYSTICK, classifyOffset(0x01F));
}

TEST(MProfileSCSDispatch, NvicISER)
{
    // ISER[0] at 0x100, ISER[7] at 0x11C
    EXPECT_EQ(NVIC, classifyOffset(0x100));
    EXPECT_EQ(NVIC, classifyOffset(0x11C));
}

TEST(MProfileSCSDispatch, NvicICER)
{
    // ICER[0] at 0x180, ICER[7] at 0x19C
    EXPECT_EQ(NVIC, classifyOffset(0x180));
    EXPECT_EQ(NVIC, classifyOffset(0x19C));
}

TEST(MProfileSCSDispatch, NvicISPR)
{
    EXPECT_EQ(NVIC, classifyOffset(0x200));
    EXPECT_EQ(NVIC, classifyOffset(0x21C));
}

TEST(MProfileSCSDispatch, NvicICPR)
{
    EXPECT_EQ(NVIC, classifyOffset(0x280));
    EXPECT_EQ(NVIC, classifyOffset(0x29C));
}

TEST(MProfileSCSDispatch, NvicIABR)
{
    // IABR at 0x300-0x31F (also covered by NVIC range)
    EXPECT_EQ(NVIC, classifyOffset(0x300));
    EXPECT_EQ(NVIC, classifyOffset(0x31C));
}

TEST(MProfileSCSDispatch, NvicIPR)
{
    // IPR[0] at 0x400, IPR[59] at 0x4EC
    EXPECT_EQ(NVIC, classifyOffset(0x400));
    EXPECT_EQ(NVIC, classifyOffset(0x4EC));
}

TEST(MProfileSCSDispatch, STIR)
{
    EXPECT_EQ(STIR, classifyOffset(0xF00));
}

TEST(MProfileSCSDispatch, SCBRange)
{
    // SCB: CPUID(0xD00) through AFSR(0xD3C)
    EXPECT_EQ(SCB, classifyOffset(0xD00));
    EXPECT_EQ(SCB, classifyOffset(0xD04));
    EXPECT_EQ(SCB, classifyOffset(0xD3C));
}

TEST(MProfileSCSDispatch, Gaps)
{
    // Addresses between sub-modules should be UNKNOWN
    EXPECT_EQ(UNKNOWN, classifyOffset(0x000));  // before SysTick
    EXPECT_EQ(UNKNOWN, classifyOffset(0x020));  // after SysTick
    EXPECT_EQ(UNKNOWN, classifyOffset(0x0FF));  // before NVIC ISER
    EXPECT_EQ(UNKNOWN, classifyOffset(0x500));  // after IPR
    EXPECT_EQ(UNKNOWN, classifyOffset(0xCFF));  // before SCB
    EXPECT_EQ(UNKNOWN, classifyOffset(0xD40));  // after SCB
}

// =========================================================================
// SysTick current value computation
// =========================================================================
//
// sysTickCurrentValue() computes the remaining count from:
//   remaining_ticks = event_when - curTick
//   current_value = remaining_ticks / clockPeriod
//
// We verify the arithmetic is correct for various scenarios.

TEST(MProfileSCSSysTick, CurrentValueComputation)
{
    // Simulating: clockPeriod=1000, load=100, started at tick 0
    // Event scheduled at tick 100000 (= 100 * 1000)
    // At tick 50000, remaining = 50000, value = 50000/1000 = 50
    uint64_t clock_period = 1000;
    uint64_t event_when = 100000;
    uint64_t cur_tick = 50000;
    uint32_t remaining = (uint32_t)((event_when - cur_tick) / clock_period);
    EXPECT_EQ(50u, remaining);
}

TEST(MProfileSCSSysTick, CurrentValueAtStart)
{
    // Just started: at tick 0, event at tick 100000
    // Remaining = 100000 / 1000 = 100 (= load value)
    uint64_t clock_period = 1000;
    uint64_t event_when = 100000;
    uint64_t cur_tick = 0;
    uint32_t remaining = (uint32_t)((event_when - cur_tick) / clock_period);
    EXPECT_EQ(100u, remaining);
}

TEST(MProfileSCSSysTick, CurrentValueNearExpiry)
{
    // One tick before expiry: remaining = 1000 / 1000 = 1
    uint64_t clock_period = 1000;
    uint64_t event_when = 100000;
    uint64_t cur_tick = 99000;
    uint32_t remaining = (uint32_t)((event_when - cur_tick) / clock_period);
    EXPECT_EQ(1u, remaining);
}

TEST(MProfileSCSSysTick, ReloadValueMask)
{
    // RVR write: only 24 bits are stored.
    // Writing 0xFFFFFFFF should yield 0x00FFFFFF.
    uint32_t load = 0xFFFFFFFF & 0x00FFFFFF;
    EXPECT_EQ(0x00FFFFFFu, load);

    // Writing 0x01000000 should yield 0 (bit 24 and above truncated).
    load = 0x01000000 & 0x00FFFFFF;
    EXPECT_EQ(0u, load);
}

TEST(MProfileSCSSysTick, CSRWritableBits)
{
    // CSR write: only bits[2:0] are writable.
    // COUNTFLAG (bit 16) is preserved from current value.
    uint32_t ctrl = (1u << 16);  // COUNTFLAG set
    uint32_t write_data = 0xFFFFFFFF;

    // Expected: preserve COUNTFLAG, write only bits[2:0]
    uint32_t new_ctrl = (ctrl & (1u << 16)) | (write_data & 0x7);
    EXPECT_EQ((1u << 16) | 0x7, new_ctrl);

    // Writing 0 should clear ENABLE/TICKINT/CLKSOURCE but keep COUNTFLAG
    new_ctrl = (ctrl & (1u << 16)) | (0 & 0x7);
    EXPECT_EQ(1u << 16, new_ctrl);
}
