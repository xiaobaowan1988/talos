/*
 * linker.ld - Linker script for ARM64 bare-metal OS
 *
 * The linker script tells the linker (ld) how to arrange the sections
 * of our compiled code in memory. Without an OS, there's no loader —
 * we must specify exactly where everything goes.
 *
 * QEMU's 'virt' machine loads the kernel image at 0x40000000 (1 GiB).
 * This is the start of RAM in the virt machine's memory map.
 *
 * Memory layout we define:
 *
 *   0x40000000  +------------------+
 *               | .text.boot       |  <- _start entry point (must be first!)
 *               | .text            |  <- rest of code
 *               +------------------+
 *               | .rodata          |  <- read-only data (string literals)
 *               +------------------+
 *               | .data            |  <- initialized global variables
 *               +------------------+
 *               | .bss             |  <- uninitialized globals (zeroed at boot)
 *               +------------------+
 *               | (16 KiB gap)     |
 *               | stack (grows ↓)  |  <- _stack_top
 *               +------------------+
 */

/* Entry point symbol - the CPU starts executing here */
ENTRY(_start)

SECTIONS
{
    /* Place everything starting at 0x40000000 (QEMU virt RAM base) */
    . = 0x40000000;

    /*
     * .text.boot MUST come first — it contains _start.
     * QEMU jumps to the beginning of the loaded image.
     */
    .text.boot : {
        *(.text.boot)
    }

    /* Regular code */
    .text : {
        *(.text*)
    }

    /* Read-only data: string literals like "Hello, World!" end up here */
    .rodata : {
        *(.rodata*)
    }

    /* Initialized data (e.g., int x = 42;) */
    .data : {
        *(.data*)
    }

    /* Align BSS to 8 bytes (64-bit alignment for AArch64) */
    . = ALIGN(8);
    _bss_start = .;

    /* Uninitialized data (e.g., int y; — will be zeroed by start.S) */
    .bss : {
        *(.bss*)
    }

    . = ALIGN(8);
    _bss_end = .;

    /*
     * Stack: 16 KiB, placed right after BSS.
     * ARM64 requires the stack to be 16-byte aligned.
     * The stack grows downward, so _stack_top is the highest address.
     */
    . = ALIGN(16);
    . = . + 0x4000;   /* 16 KiB stack */
    _stack_top = .;
}
