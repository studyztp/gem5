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

# Source files
C_SRCS  = main_freertos_test.c \
          $(KERNEL)/tasks.c \
          $(KERNEL)/list.c \
          $(KERNEL)/queue.c \
          $(KERNEL)/portable/GCC/ARM_CM3/port.c \
          $(KERNEL)/portable/MemMang/heap_1.c

ASM_SRCS = startup_freertos.S

OBJS    = $(C_SRCS:.c=.o) $(ASM_SRCS:.S=.o)
TARGET  = freertos_test.elf

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(LDFLAGS) -o $@ $^
	$(OBJDUMP) -d $@ > freertos_test.dump

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

%.o: %.S
	$(CC) $(ASFLAGS) -c -o $@ $<

clean:
	rm -f $(OBJS) $(TARGET) freertos_test.dump
