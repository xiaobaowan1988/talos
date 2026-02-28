/*
 * kernel.c - The main kernel of our Hello World OS
 *
 * This is the C entry point called by start.S after hardware setup.
 * It demonstrates the minimum viable bare-metal program:
 * just print a message through the UART serial port.
 */

#include "uart.h"

/*
 * kernel_main - The heart of our tiny OS
 *
 * This function is called from start.S after:
 *   - The stack has been set up
 *   - The .bss section has been zeroed
 *
 * We simply print "Hello, World!" to the UART and return.
 * When we return, start.S will halt the CPU.
 */
void kernel_main(void)
{
    uart_puts("\n");
    uart_puts("========================================\n");
    uart_puts("  Hello, World!\n");
    uart_puts("  ARM64 (AArch64/ARMv8) Bare-Metal OS\n");
    uart_puts("  Running on QEMU virt machine\n");
    uart_puts("========================================\n");
    uart_puts("\n");
    uart_puts("This message is sent through PL011 UART\n");
    uart_puts("at address 0x09000000.\n");
    uart_puts("\n");
    uart_puts("System halting...\n");
}
