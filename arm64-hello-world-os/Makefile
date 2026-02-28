# Makefile for ARM64 bare-metal Hello World OS
#
# Cross-compilation: We are compiling ON x86_64 (or any host)
# FOR aarch64 (ARM64). This requires a cross-compiler toolchain.
#
# Tool explanations:
#   aarch64-linux-gnu-gcc  - Cross-compiler: compiles C to ARM64 machine code
#   aarch64-linux-gnu-as   - Cross-assembler: assembles ARM64 assembly
#   aarch64-linux-gnu-ld   - Cross-linker: links object files into a binary
#   aarch64-linux-gnu-objcopy - Converts ELF to raw binary image

CROSS = aarch64-linux-gnu-

CC      = $(CROSS)gcc
AS      = $(CROSS)as
LD      = $(CROSS)ld
OBJCOPY = $(CROSS)objcopy

# Compiler flags explained:
#   -ffreestanding    : Don't assume a hosted environment (no libc, no main())
#   -nostdlib         : Don't link against standard library
#   -nostartfiles     : Don't use standard startup files (crt0.o etc.)
#   -Wall -Wextra     : Enable comprehensive warnings
#   -O2               : Optimize for speed (safe for bare-metal)
#   -march=armv8-a    : Target ARMv8-A architecture
CFLAGS  = -ffreestanding -nostdlib -nostartfiles -Wall -Wextra -O2 -march=armv8-a

# Linker flags:
#   -T linker.ld      : Use our custom linker script
#   -nostdlib          : Don't link standard libraries
LDFLAGS = -T linker.ld -nostdlib

# Source files
ASM_SRC = start.S
C_SRC   = kernel.c

# Object files
OBJS    = start.o kernel.o

# Output files
ELF     = kernel.elf
IMG     = kernel.img

.PHONY: all clean run run-debug

# Default target: build everything
all: $(IMG)

# Assemble start.S -> start.o
start.o: start.S
	$(CC) $(CFLAGS) -c $< -o $@

# Compile kernel.c -> kernel.o
kernel.o: kernel.c uart.h
	$(CC) $(CFLAGS) -c $< -o $@

# Link all objects into an ELF executable
$(ELF): $(OBJS) linker.ld
	$(LD) $(LDFLAGS) $(OBJS) -o $@

# Convert ELF to raw binary image
# Why? QEMU -kernel expects a raw binary or ELF, but raw binary is simpler
# and guaranteed to start at the right address without ELF header parsing.
$(IMG): $(ELF)
	$(OBJCOPY) -O binary $< $@

# Run in QEMU
#   -machine virt    : Use ARM's virtual platform (no real hardware to emulate)
#   -cpu cortex-a53  : Emulate a Cortex-A53 CPU (common ARMv8 CPU)
#   -nographic       : No GUI window, serial output goes to terminal
#   -kernel          : Load our kernel image
run: $(IMG)
	qemu-system-aarch64 \
		-machine virt \
		-cpu cortex-a53 \
		-nographic \
		-kernel $(IMG)

# Run with GDB debug server (useful for debugging with gdb-multiarch)
run-debug: $(IMG)
	qemu-system-aarch64 \
		-machine virt \
		-cpu cortex-a53 \
		-nographic \
		-kernel $(IMG) \
		-s -S

clean:
	rm -f $(OBJS) $(ELF) $(IMG)
