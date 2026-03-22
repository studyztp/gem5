# Makefile for FreeRTOS test on gem5 M-profile (STM32F405)
#
# Usage:
#   make              # build freertos_test.elf
#   make clean        # remove build artifacts

CC      = arm-none-eabi-gcc
OBJDUMP = arm-none-eabi-objdump

# FreeRTOS kernel path (cloned in this directory)
KERNEL  = FreeRTOS-Kernel

CFLAGS  = -mcpu=cortex-m4 -mthumb -mfloat-abi=soft \
          -O1 -g -Wall -Wextra -Wno-unused-parameter \
          -ffunction-sections -fdata-sections \
          -I. \
          -I$(KERNEL)/include \
          -I$(KERNEL)/portable/GCC/ARM_CM3

ASFLAGS = -mcpu=cortex-m4 -mthumb -mfloat-abi=soft -g

LDFLAGS = -mcpu=cortex-m4 -mthumb -mfloat-abi=soft \
          -T freertos.ld -nostartfiles \
          -Wl,--gc-sections \
          -specs=nosys.specs

# Shared FreeRTOS kernel sources
KERNEL_SRCS = $(KERNEL)/tasks.c \
              $(KERNEL)/list.c \
              $(KERNEL)/queue.c \
              $(KERNEL)/portable/GCC/ARM_CM3/port.c \
              $(KERNEL)/portable/MemMang/heap_1.c

ASM_SRCS = startup_freertos.S

# --- Target 1: freertos_test.elf (original two-task test) ---
BOOT_C_SRCS  = main_freertos_test.c $(KERNEL_SRCS)
BOOT_OBJS    = $(BOOT_C_SRCS:.c=.o) $(ASM_SRCS:.S=.o)

# --- Target 2: freertos_icsr_test.elf (BUG-2 ICSR field test) ---
# Needs configUSE_TICK_HOOK=1.  We override via -D on the compiler
# command line, which takes precedence over the #define in FreeRTOSConfig.h.
ICSR_CFLAGS  = $(CFLAGS) -DconfigUSE_TICK_HOOK=1
ICSR_C_SRCS  = main_freertos_icsr_test.c $(KERNEL_SRCS)
# Build ICSR test objects into a separate directory to avoid conflicts
ICSR_OBJDIR  = icsr_build
ICSR_C_OBJS  = $(addprefix $(ICSR_OBJDIR)/,$(notdir $(ICSR_C_SRCS:.c=.o)))
ICSR_ASM_OBJS = $(addprefix $(ICSR_OBJDIR)/,$(notdir $(ASM_SRCS:.S=.o)))
ICSR_OBJS    = $(ICSR_C_OBJS) $(ICSR_ASM_OBJS)

.PHONY: all clean

all: freertos_test.elf freertos_icsr_test.elf

freertos_test.elf: $(BOOT_OBJS)
	$(CC) $(LDFLAGS) -o $@ $^
	$(OBJDUMP) -d $@ > freertos_test.dump

freertos_icsr_test.elf: $(ICSR_OBJS)
	$(CC) $(LDFLAGS) -o $@ $^
	$(OBJDUMP) -d $@ > freertos_icsr_test.dump

# Default rules for freertos_test objects
%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

%.o: %.S
	$(CC) $(ASFLAGS) -c -o $@ $<

# ICSR test objects — built with tick hook enabled, in separate directory
$(ICSR_OBJDIR):
	mkdir -p $(ICSR_OBJDIR)

$(ICSR_OBJDIR)/%.o: %.c | $(ICSR_OBJDIR)
	$(CC) $(ICSR_CFLAGS) -c -o $@ $<

$(ICSR_OBJDIR)/%.o: %.S | $(ICSR_OBJDIR)
	$(CC) $(ASFLAGS) -c -o $@ $<

$(ICSR_OBJDIR)/%.o: $(KERNEL)/%.c | $(ICSR_OBJDIR)
	$(CC) $(ICSR_CFLAGS) -c -o $@ $<

$(ICSR_OBJDIR)/%.o: $(KERNEL)/portable/GCC/ARM_CM3/%.c | $(ICSR_OBJDIR)
	$(CC) $(ICSR_CFLAGS) -c -o $@ $<

$(ICSR_OBJDIR)/%.o: $(KERNEL)/portable/MemMang/%.c | $(ICSR_OBJDIR)
	$(CC) $(ICSR_CFLAGS) -c -o $@ $<

clean:
	rm -f $(BOOT_OBJS) freertos_test.elf freertos_test.dump
	rm -rf $(ICSR_OBJDIR) freertos_icsr_test.elf freertos_icsr_test.dump
