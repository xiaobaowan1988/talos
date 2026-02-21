/*
 * linker.ld — ARM64 kernel linker script
 *
 * QEMU virt machine RAM starts at 0x40000000.
 * We place ourselves at 0x40000000 (flat load via -kernel).
 *
 * Memory layout after link:
 *   0x40000000  .text   (entry.S first, then all code)
 *   ...         .rodata
 *   ...         .data
 *   ...         .bss
 *   ...         _stack_bottom
 *   ...+16KB    _stack_top   (initial kernel stack)
 */

ENTRY(_start)

SECTIONS
{
    . = 0x40000000;

    .text : {
        boot/entry.o(.text)     /* entry point must be first */
        *(.text .text.*)
    }

    . = ALIGN(4096);
    .rodata : {
        *(.rodata .rodata.*)
    }

    . = ALIGN(4096);
    .data : {
        *(.data .data.*)
    }

    . = ALIGN(16);
    .bss : {
        _bss_start = .;
        *(.bss .bss.*)
        *(COMMON)
        . = ALIGN(16);
        _bss_end = .;
    }

    /* Initial kernel stack: 16KB */
    . = ALIGN(4096);
    _stack_bottom = .;
    . += (4096 * 4);
    _stack_top = .;
}
