# Makefile for Cortex-M4 gem5 test programs
#
# Requires: arm-none-eabi-gcc (ARM embedded toolchain)
# Install: apt install gcc-arm-none-eabi
#
# Usage:
#   make                    # build all test ELFs
#   make clean              # remove build artifacts
#   make test_cps.elf       # build specific test
#
# Run a test:
#   ./build/ALL/gem5.opt tests/gem5/m_profile_tests/configs/run_m4_test.py \
#       --firmware tests/gem5/m_profile_tests/programs/test_cps.elf

CROSS = arm-none-eabi-
AS = $(CROSS)as
LD = $(CROSS)ld
OBJDUMP = $(CROSS)objdump

ASFLAGS = -mcpu=cortex-m4 -mthumb
LDFLAGS = -T cortexm4.ld --no-warn-rwx-segments

TESTS = cortexm4_basic test_cps test_mrs_msr test_bx_exc_return \
        test_load_store test_blocked_insts test_sp_sync \
        test_pop_pc_exc_return test_nested_exceptions \
        test_data_processing

all: $(addsuffix .elf, $(TESTS))

%.o: %.S
	$(AS) $(ASFLAGS) $< -o $@

%.elf: %.o cortexm4.ld
	$(LD) $(LDFLAGS) $< -o $@
	$(OBJDUMP) -d -t $@ > $(@:.elf=.dump)

clean:
	rm -f *.o *.elf *.dump

.PHONY: all clean
