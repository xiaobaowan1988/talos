# Makefile — ARM64v8 OS build system
#
# Cross-compilation with aarch64-linux-gnu toolchain.
# Targets:
#   make          → build kernel.elf
#   make run      → launch in QEMU (Ctrl-A X to quit)
#   make debug    → launch QEMU + GDB paused at _start
#   make dis      → disassemble kernel (readable listing)
#   make clean    → remove build artefacts

# ── Toolchain ─────────────────────────────────────────────────────────────
CROSS   := aarch64-linux-gnu-
CC      := $(CROSS)gcc
LD      := $(CROSS)ld
OBJDUMP := $(CROSS)objdump
OBJCOPY := $(CROSS)objcopy
NM      := $(CROSS)nm

# ── Compiler flags ────────────────────────────────────────────────────────
# -nostdlib      no libc/libgcc linking
# -nostdinc      no host system headers
# -ffreestanding no assumption of standard environment
# -mstrict-align no unaligned accesses (required for early boot)
# -march=armv8-a target ARMv8-A baseline (all Cortex-A5x, A7x, Neoverse)
CFLAGS  := \
    -nostdlib          \
    -nostdinc          \
    -ffreestanding     \
    -fno-stack-protector \
    -fno-pie           \
    -march=armv8-a     \
    -mstrict-align     \
    -O1                \
    -g                 \
    -Wall              \
    -Wextra            \
    -I include

ASFLAGS := \
    -march=armv8-a     \
    -g

LDFLAGS := -nostdlib -T linker.ld

# ── Sources → objects ─────────────────────────────────────────────────────
SRCS_S  := boot/entry.S
SRCS_C  := drivers/uart.c                   \
           drivers/irqchip/gic.c            \
           drivers/clocksource/timer.c      \
           mm/memblock.c                    \
           mm/mmu.c                         \
           mm/page_alloc.c                  \
           mm/slab.c                        \
           kernel/irq.c                     \
           init/main.c

OBJS    := $(SRCS_S:.S=.o) $(SRCS_C:.c=.o)

# ── QEMU ──────────────────────────────────────────────────────────────────
# -machine virt       QEMU generic ARM virtual machine
# -cpu cortex-a72     ARMv8-A, boots at EL2
# -m 128M             128 MB RAM (starts at 0x40000000)
# -nographic          serial → stdio (no window)
# -kernel kernel.elf  load ELF, set PC to entry point
QEMU    := qemu-system-aarch64
QFLAGS  := \
    -machine virt,gic-version=3 \
    -cpu cortex-a72             \
    -m 128M                \
    -nographic             \
    -kernel kernel.elf

# ── Build targets ─────────────────────────────────────────────────────────
.PHONY: all run debug dis syms clean

all: kernel.elf

kernel.elf: $(OBJS) linker.ld
	$(LD) $(LDFLAGS) -o $@ $(OBJS)
	@echo ""
	@echo "  Built: $@"
	@$(CROSS)size $@

# Assemble .S files — use CC so we get preprocessor macros
boot/entry.o: boot/entry.S
	$(CC) $(CFLAGS) $(ASFLAGS) -c $< -o $@

# Compile .c files
%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

# ── Run in QEMU ───────────────────────────────────────────────────────────
# Press Ctrl-A then X to exit QEMU
run: kernel.elf
	@echo "Starting QEMU — press Ctrl-A X to exit"
	@echo "─────────────────────────────────────────"
	$(QEMU) $(QFLAGS)

# ── Debug with GDB ────────────────────────────────────────────────────────
# Terminal 1: make debug-qemu   → QEMU waits for GDB
# Terminal 2: make debug-gdb    → GDB connects and breaks at _start
debug-qemu: kernel.elf
	$(QEMU) $(QFLAGS) -s -S

debug-gdb: kernel.elf
	gdb-multiarch kernel.elf \
	    -ex "set arch aarch64"  \
	    -ex "target remote :1234" \
	    -ex "break kernel_main" \
	    -ex "continue"

# ── Disassembly ───────────────────────────────────────────────────────────
dis: kernel.elf
	$(OBJDUMP) -d -S kernel.elf | less

# ── Symbol table ──────────────────────────────────────────────────────────
syms: kernel.elf
	$(NM) -n kernel.elf

# ── Clean ─────────────────────────────────────────────────────────────────
clean:
	rm -f $(OBJS) kernel.elf
	rm -f mm/*.o kernel/*.o
	rm -f drivers/irqchip/*.o drivers/clocksource/*.o
