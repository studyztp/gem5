#!/usr/bin/env python3
#
# Copyright (c) 2026 University of California, Davis and Cornell University
# All rights reserved
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
Unit tests for M-profile SimObject configuration.

Tests that all M-profile SimObjects (MProfileSCS, MProfileInterrupts,
ArmMCPU variants, ArmMSystem, ArmMISA, ArmMMMU) can be imported,
instantiated, and have correct default parameter values.

Run with:
    ./build/<target>/gem5.opt tests/run_pyunit.py
"""

import unittest


class TestMProfileImports(unittest.TestCase):
    """Verify all M-profile SimObjects can be imported."""

    def test_import_arm_m_system(self):
        from m5.objects.ArmMSystem import (
            ArmMRelease,
            ArmMSystem,
        )

        self.assertIsNotNone(ArmMSystem)
        self.assertIsNotNone(ArmMRelease)

    def test_import_arm_m_isa(self):
        from m5.objects.ArmMISA import ArmMISA

        self.assertIsNotNone(ArmMISA)

    def test_import_arm_m_mmu(self):
        from m5.objects.ArmMMMU import (
            ArmMMMU,
            ArmMTLB,
        )

        self.assertIsNotNone(ArmMMMU)
        self.assertIsNotNone(ArmMTLB)

    def test_import_arm_m_interrupts(self):
        from m5.objects.ArmMInterrupts import MProfileInterrupts

        self.assertIsNotNone(MProfileInterrupts)

    def test_import_arm_m_cpu(self):
        from m5.objects.ArmMCPU import (
            ArmMAtomicSimpleCPU,
            ArmMMinorCPU,
            ArmMTimingSimpleCPU,
        )

        self.assertIsNotNone(ArmMAtomicSimpleCPU)
        self.assertIsNotNone(ArmMTimingSimpleCPU)
        self.assertIsNotNone(ArmMMinorCPU)

    def test_import_m_profile_scs(self):
        from m5.objects.MProfileSCS import MProfileSCS

        self.assertIsNotNone(MProfileSCS)

    def test_import_arm_m_fs_workload(self):
        from m5.objects.ArmMFsWorkload import ArmMFsWorkload

        self.assertIsNotNone(ArmMFsWorkload)


class TestMProfileSCSParams(unittest.TestCase):
    """Verify MProfileSCS parameter defaults and validation."""

    def setUp(self):
        from m5.objects.MProfileSCS import MProfileSCS

        self.scs = MProfileSCS()

    def test_default_pio_addr(self):
        """SCS base address must be 0xE000E000 per ARMv7-M architecture."""
        self.assertEqual(int(self.scs.pio_addr), 0xE000E000)

    def test_default_num_irqs(self):
        """Default 32 external IRQs (suitable for M0/M0+)."""
        self.assertEqual(int(self.scs.num_irqs), 32)

    def test_default_priority_bits(self):
        """Default 4 priority bits (16 levels, suitable for M4)."""
        self.assertEqual(int(self.scs.priority_bits), 4)

    def test_default_has_systick(self):
        """SysTick present by default (mandatory on M3/M4/M7)."""
        self.assertTrue(bool(self.scs.has_systick))

    def test_default_has_basepri(self):
        """BASEPRI/FAULTMASK available by default (M3/M4/M7)."""
        self.assertTrue(bool(self.scs.has_basepri))

    def test_default_systick_calib(self):
        """SysTick CALIB defaults to 0 (calibration not known)."""
        self.assertEqual(int(self.scs.systick_calib), 0)

    def test_m0_configuration(self):
        """Verify M0/M0+ configuration can be expressed."""
        from m5.objects.MProfileSCS import MProfileSCS

        scs = MProfileSCS(
            num_irqs=32,
            priority_bits=2,
            has_systick=False,
            has_basepri=False,
        )
        self.assertEqual(int(scs.num_irqs), 32)
        self.assertEqual(int(scs.priority_bits), 2)
        self.assertFalse(bool(scs.has_systick))
        self.assertFalse(bool(scs.has_basepri))

    def test_m4_configuration(self):
        """Verify M4 configuration can be expressed."""
        from m5.objects.MProfileSCS import MProfileSCS

        scs = MProfileSCS(
            num_irqs=240,
            priority_bits=4,
            has_systick=True,
            has_basepri=True,
        )
        self.assertEqual(int(scs.num_irqs), 240)
        self.assertEqual(int(scs.priority_bits), 4)
        self.assertTrue(bool(scs.has_systick))
        self.assertTrue(bool(scs.has_basepri))


class TestArmMISAParams(unittest.TestCase):
    """Verify ArmMISA parameter defaults."""

    def test_default_vtor_align_bits(self):
        """Default VTOR alignment is 9 (512-byte, for M4/M7)."""
        from m5.objects.ArmMISA import ArmMISA

        isa = ArmMISA()
        self.assertEqual(int(isa.vtor_align_bits), 9)

    def test_m0_vtor_alignment(self):
        """M0/M0+ uses 7-bit VTOR alignment (128-byte)."""
        from m5.objects.ArmMISA import ArmMISA

        isa = ArmMISA(vtor_align_bits=7)
        self.assertEqual(int(isa.vtor_align_bits), 7)


class TestArmMSystemParams(unittest.TestCase):
    """Verify ArmMSystem configuration."""

    def test_default_release_has_m_profile(self):
        """Default release must include M_PROFILE extension."""
        from m5.objects.ArmMSystem import ArmMRelease

        release = ArmMRelease()
        # extensions contains gem5 param wrapper objects, convert to strings
        ext_names = [str(e) for e in release.extensions]
        self.assertIn("M_PROFILE", ext_names)

    def test_scs_param_default_null(self):
        """SCS param should default to NULL (set during init())."""
        from m5.objects.ArmMSystem import ArmMSystem

        sys_obj = ArmMSystem()
        # gem5 represents NULL params as the Python None-like NULL object.
        # Check that it's not set to a real MProfileSCS instance.
        from m5.objects.MProfileSCS import MProfileSCS

        self.assertNotIsInstance(sys_obj.scs, MProfileSCS)


class TestArmMCPUBindings(unittest.TestCase):
    """Verify ArmMCPU mixin binds correct architecture components."""

    def test_atomic_cpu_arch_bindings(self):
        """ArmMAtomicSimpleCPU should use M-profile components."""
        from m5.objects.ArmMCPU import ArmMAtomicSimpleCPU
        from m5.objects.ArmMDecoder import ArmMDecoder
        from m5.objects.ArmMInterrupts import MProfileInterrupts
        from m5.objects.ArmMISA import ArmMISA
        from m5.objects.ArmMMMU import ArmMMMU

        cpu = ArmMAtomicSimpleCPU()

        # Verify the mixin set the correct Arch* types
        self.assertEqual(cpu.ArchISA, ArmMISA)
        self.assertEqual(cpu.ArchMMU, ArmMMMU)
        self.assertEqual(cpu.ArchInterrupts, MProfileInterrupts)
        self.assertEqual(cpu.ArchDecoder, ArmMDecoder)

    def test_timing_cpu_arch_bindings(self):
        """ArmMTimingSimpleCPU should use M-profile components."""
        from m5.objects.ArmMCPU import ArmMTimingSimpleCPU
        from m5.objects.ArmMISA import ArmMISA

        cpu = ArmMTimingSimpleCPU()
        self.assertEqual(cpu.ArchISA, ArmMISA)

    def test_minor_cpu_arch_bindings(self):
        """ArmMMinorCPU should use M-profile components."""
        from m5.objects.ArmMCPU import ArmMMinorCPU
        from m5.objects.ArmMISA import ArmMISA

        cpu = ArmMMinorCPU()
        self.assertEqual(cpu.ArchISA, ArmMISA)

    def test_cpu_has_m_profile_mmu(self):
        """CPU's mmu instance should be ArmMMMU."""
        from m5.objects.ArmMCPU import ArmMAtomicSimpleCPU
        from m5.objects.ArmMMMU import ArmMMMU

        cpu = ArmMAtomicSimpleCPU()
        self.assertIsInstance(cpu.mmu, ArmMMMU)


class TestMProfileInterruptsParams(unittest.TestCase):
    """Verify MProfileInterrupts instantiation."""

    def test_instantiation(self):
        """MProfileInterrupts should instantiate with no params."""
        from m5.objects.ArmMInterrupts import MProfileInterrupts

        intr = MProfileInterrupts()
        self.assertIsNotNone(intr)


# =====================================================================
# Step 7: Platform and Board tests
# =====================================================================


class TestArmMPlatformImport(unittest.TestCase):
    """Verify ArmMPlatform SimObject can be imported and instantiated."""

    def test_import(self):
        from m5.objects.MProfilePlatform import ArmMPlatform

        self.assertIsNotNone(ArmMPlatform)

    def test_instantiation(self):
        from m5.objects.MProfilePlatform import ArmMPlatform

        platform = ArmMPlatform()
        self.assertIsNotNone(platform)


class TestArmMPlatformDefaults(unittest.TestCase):
    """Verify ArmMPlatform default parameter values."""

    def setUp(self):
        from m5.objects.MProfilePlatform import ArmMPlatform

        self.platform = ArmMPlatform()

    def test_default_code_ranges_not_empty(self):
        """Default code_ranges should have at least one flash region."""
        self.assertGreater(len(self.platform.code_ranges), 0)

    def test_default_sram_ranges_not_empty(self):
        """Default sram_ranges should have at least one SRAM region."""
        self.assertGreater(len(self.platform.sram_ranges), 0)

    def test_default_periph_ranges_not_empty(self):
        """Default periph_ranges should have at least one region."""
        self.assertGreater(len(self.platform.periph_ranges), 0)

    def test_default_external_ram_empty(self):
        """External RAM should be empty by default (not all chips have it)."""
        self.assertEqual(len(self.platform.external_ram_ranges), 0)

    def test_default_external_device_empty(self):
        """External device should be empty by default."""
        self.assertEqual(len(self.platform.external_device_ranges), 0)

    def test_default_vendor_ranges_empty(self):
        """Vendor ranges should be empty by default."""
        self.assertEqual(len(self.platform.vendor_ranges), 0)

    def test_default_boot_alias_empty(self):
        """Boot alias should be empty by default (not all chips need it)."""
        self.assertEqual(len(self.platform.boot_alias_ranges), 0)

    def test_has_scs(self):
        """Platform should have an SCS device."""
        from m5.objects.MProfileSCS import MProfileSCS

        self.assertIsNotNone(self.platform.scs)


class TestSTM32F405Platform(unittest.TestCase):
    """Verify STM32F405 pre-configured platform."""

    def setUp(self):
        from gem5.prebuilt.cortexm.platforms import STM32F405Platform

        self.platform = STM32F405Platform()

    def test_flash_at_0x08000000(self):
        """STM32F405 flash should start at 0x08000000."""
        self.assertEqual(len(self.platform.code_ranges), 1)
        flash = self.platform.code_ranges[0]
        self.assertEqual(int(flash.start), 0x08000000)

    def test_three_sram_blocks(self):
        """STM32F405 has 3 SRAM blocks: SRAM1, SRAM2, CCM."""
        self.assertEqual(len(self.platform.sram_ranges), 3)

    def test_sram1_at_0x20000000(self):
        """SRAM1 should start at 0x20000000."""
        sram1 = self.platform.sram_ranges[0]
        self.assertEqual(int(sram1.start), 0x20000000)

    def test_ccm_at_0x10000000(self):
        """CCM SRAM should start at 0x10000000 (outside SRAM region)."""
        ccm = self.platform.sram_ranges[2]
        self.assertEqual(int(ccm.start), 0x10000000)

    def test_scs_82_irqs(self):
        """STM32F405 has 82 NVIC IRQs."""
        self.assertEqual(int(self.platform.scs.num_irqs), 82)

    def test_scs_4bit_priority(self):
        """STM32F405 has 4-bit priority (16 levels)."""
        self.assertEqual(int(self.platform.scs.priority_bits), 4)

    def test_boot_alias(self):
        """STM32F405 has boot alias at 0x00000000."""
        self.assertEqual(len(self.platform.boot_alias_ranges), 1)
        alias = self.platform.boot_alias_ranges[0]
        self.assertEqual(int(alias.start), 0x00000000)

    def test_default_memories(self):
        """default_memories() should return 4 memories (flash+3 SRAMs)."""
        memories = self.platform.default_memories()
        self.assertEqual(len(memories), 4)


class TestSTM32G474REPlatform(unittest.TestCase):
    """Verify STM32G474RE pre-configured platform."""

    def setUp(self):
        from gem5.prebuilt.cortexm.platforms import STM32G474REPlatform

        self.platform = STM32G474REPlatform()

    def test_dual_bank_flash(self):
        """STM32G474RE has dual-bank flash (2 entries)."""
        self.assertEqual(len(self.platform.code_ranges), 2)

    def test_flash_bank1_at_0x08000000(self):
        """Flash Bank 1 at 0x08000000."""
        bank1 = self.platform.code_ranges[0]
        self.assertEqual(int(bank1.start), 0x08000000)

    def test_flash_bank2_at_0x08040000(self):
        """Flash Bank 2 at 0x08040000."""
        bank2 = self.platform.code_ranges[1]
        self.assertEqual(int(bank2.start), 0x08040000)

    def test_three_sram_blocks(self):
        """STM32G474RE has 3 SRAM blocks: SRAM1, SRAM2, CCM."""
        self.assertEqual(len(self.platform.sram_ranges), 3)

    def test_scs_102_irqs(self):
        """STM32G474RE has 102 NVIC IRQs."""
        self.assertEqual(int(self.platform.scs.num_irqs), 102)

    def test_default_memories(self):
        """default_memories() should return 5 memories (2 flash + 3 SRAMs)."""
        memories = self.platform.default_memories()
        self.assertEqual(len(memories), 5)


# =====================================================================
# Step 8: Decoder and Release tests
# =====================================================================


class TestArmMDecoderImport(unittest.TestCase):
    """Verify ArmMDecoder SimObject can be imported and instantiated."""

    def test_import(self):
        from m5.objects.ArmMDecoder import ArmMDecoder

        self.assertIsNotNone(ArmMDecoder)

    def test_instantiation(self):
        from m5.objects.ArmMDecoder import ArmMDecoder
        from m5.objects.ArmMISA import ArmMISA

        # ArmMDecoder inherits from ArmDecoder and needs an ISA param
        decoder = ArmMDecoder(isa=ArmMISA())
        self.assertIsNotNone(decoder)


class TestArmMCPUUsesDecoder(unittest.TestCase):
    """Verify ArmMCPU mixin uses ArmMDecoder, not ArmDecoder."""

    def test_arch_decoder_is_m_decoder(self):
        """ArmMCPU.ArchDecoder must be ArmMDecoder (not ArmDecoder)."""
        from m5.objects.ArmMCPU import ArmMAtomicSimpleCPU
        from m5.objects.ArmMDecoder import ArmMDecoder

        self.assertEqual(ArmMAtomicSimpleCPU.ArchDecoder, ArmMDecoder)


class TestArmMReleaseHierarchy(unittest.TestCase):
    """Verify M-profile release class extension hierarchy."""

    def test_base_release(self):
        """ArmMRelease has only M_PROFILE."""
        from m5.objects.ArmMSystem import ArmMRelease

        release = ArmMRelease()
        ext_names = [str(e) for e in release.extensions]
        self.assertIn("M_PROFILE", ext_names)

    def test_cortex_m0_release(self):
        """CortexM0 has M_PROFILE + ARMV6M, no ARMV7M."""
        from m5.objects.ArmMSystem import ArmMReleaseCortexM0

        release = ArmMReleaseCortexM0()
        ext_names = [str(e) for e in release.extensions]
        self.assertIn("M_PROFILE", ext_names)
        self.assertIn("M_PROFILE_ARMV6M", ext_names)
        self.assertNotIn("M_PROFILE_ARMV7M", ext_names)
        self.assertNotIn("M_PROFILE_DSP", ext_names)

    def test_cortex_m3_release(self):
        """CortexM3 has ARMV6M + ARMV7M, no DSP/FPU."""
        from m5.objects.ArmMSystem import ArmMReleaseCortexM3

        release = ArmMReleaseCortexM3()
        ext_names = [str(e) for e in release.extensions]
        self.assertIn("M_PROFILE_ARMV6M", ext_names)
        self.assertIn("M_PROFILE_ARMV7M", ext_names)
        self.assertNotIn("M_PROFILE_ARMV7EM", ext_names)
        self.assertNotIn("M_PROFILE_DSP", ext_names)

    def test_cortex_m4_release(self):
        """CortexM4 release has full stack: ARMV7EM + DSP + FPU_SP."""
        from m5.objects.ArmMSystem import ArmMReleaseCortexM4

        release = ArmMReleaseCortexM4()
        ext_names = [str(e) for e in release.extensions]
        self.assertIn("M_PROFILE", ext_names)
        self.assertIn("M_PROFILE_ARMV6M", ext_names)
        self.assertIn("M_PROFILE_ARMV7M", ext_names)
        self.assertIn("M_PROFILE_ARMV7EM", ext_names)
        self.assertIn("M_PROFILE_DSP", ext_names)
        self.assertIn("M_PROFILE_FPU_SP", ext_names)
        self.assertNotIn("M_PROFILE_FPU_DP", ext_names)

    def test_cortex_m4_no_fpu_release(self):
        """CortexM4NoFPU has DSP but no FPU."""
        from m5.objects.ArmMSystem import ArmMReleaseCortexM4NoFPU

        release = ArmMReleaseCortexM4NoFPU()
        ext_names = [str(e) for e in release.extensions]
        self.assertIn("M_PROFILE_DSP", ext_names)
        self.assertNotIn("M_PROFILE_FPU_SP", ext_names)

    def test_cortex_m7_release(self):
        """CortexM7 has everything M4 has plus FPU_DP."""
        from m5.objects.ArmMSystem import ArmMReleaseCortexM7

        release = ArmMReleaseCortexM7()
        ext_names = [str(e) for e in release.extensions]
        self.assertIn("M_PROFILE_FPU_SP", ext_names)
        self.assertIn("M_PROFILE_FPU_DP", ext_names)

    def test_hierarchy_is_cumulative(self):
        """Each level inherits all extensions from parent."""
        from m5.objects.ArmMSystem import (
            ArmMReleaseCortexM0,
            ArmMReleaseCortexM3,
            ArmMReleaseCortexM4,
            ArmMReleaseCortexM7,
        )

        m0_exts = {str(e) for e in ArmMReleaseCortexM0().extensions}
        m3_exts = {str(e) for e in ArmMReleaseCortexM3().extensions}
        m4_exts = {str(e) for e in ArmMReleaseCortexM4().extensions}
        m7_exts = {str(e) for e in ArmMReleaseCortexM7().extensions}

        # Each level is a superset of the previous
        self.assertTrue(m0_exts.issubset(m3_exts))
        self.assertTrue(m3_exts.issubset(m4_exts))
        self.assertTrue(m4_exts.issubset(m7_exts))


if __name__ == "__main__":
    unittest.main()
