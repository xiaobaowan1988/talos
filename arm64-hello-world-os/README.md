# ARM64 (AArch64/ARMv8) Hello World OS

A minimal bare-metal operating system for ARM64 that prints "Hello, World!" through the UART serial port, running on QEMU.

## File Structure

```
arm64-hello-world-os/
├── start.S      # Assembly entry point: stack setup, BSS zeroing, jump to C
├── uart.h       # PL011 UART driver (serial port output)
├── kernel.c     # C kernel main: prints "Hello, World!"
├── linker.ld    # Linker script: memory layout definition
├── Makefile     # Build system
├── run.sh       # Convenience build & run script
└── README.md    # This file
```

## Quick Start

```bash
# Install dependencies (Ubuntu/Debian)
sudo apt-get install gcc-aarch64-linux-gnu qemu-system-arm

# Build and run
make
make run

# Or use the convenience script
./run.sh
```

Press `Ctrl-A` then `X` to exit QEMU.

## How It Works — The Complete Boot Sequence

### 1. Power On → CPU starts at `_start` (start.S)

When QEMU starts with `-machine virt -kernel kernel.img`:
- QEMU loads `kernel.img` into memory at `0x40000000` (the RAM base of the `virt` machine)
- The CPU begins executing from the first instruction of the image

`_start` does three things:
1. **Sets up the stack pointer** — ARM64 stack grows downward; we point SP to `_stack_top`
2. **Zeros the .bss section** — C standard requires uninitialized globals to be zero
3. **Calls `kernel_main()`** — Branch to our C code

### 2. kernel_main() runs (kernel.c)

The C function sends strings to the UART using `uart_puts()`.

### 3. UART output (uart.h)

The PL011 UART at `0x09000000` converts our byte writes into serial output.
For each character:
1. Wait until the TX FIFO has space (check UARTFR bit 5)
2. Write the character to UARTDR

### 4. Halt

After `kernel_main()` returns, `start.S` enters an infinite `wfe` loop (Wait For Event), putting the CPU in low-power idle.

## Key Concepts Explained

### Why `-ffreestanding`?
We have no operating system, no C standard library, no `printf()`. The `-ffreestanding` flag tells GCC: "don't assume any of the standard runtime exists."

### Why a custom linker script?
Without an OS loader, we must tell the linker exactly where to place code in physical memory. The `virt` machine's RAM starts at `0x40000000`, so our code must be placed there.

### Why `volatile` for UART access?
Hardware registers are not normal memory. Reading/writing them has side effects (e.g., writing UARTDR sends a character). `volatile` prevents the compiler from optimizing away these accesses or reordering them.

### What is the PL011?
ARM's PrimeCell UART (PL011) is a standard serial port controller used in many ARM systems. QEMU's `virt` machine emulates one at `0x09000000`.

## QEMU Command Explained

```
qemu-system-aarch64 \
    -machine virt \      # ARM virtual platform (no real HW to emulate)
    -cpu cortex-a53 \    # ARMv8-A CPU (64-bit)
    -nographic \         # No GUI; serial I/O via terminal
    -kernel kernel.img   # Our bare-metal binary
```

## Debugging

```bash
# Start QEMU with GDB server (halted at first instruction)
make run-debug

# In another terminal, connect with GDB
gdb-multiarch kernel.elf -ex 'target remote :1234'
```
