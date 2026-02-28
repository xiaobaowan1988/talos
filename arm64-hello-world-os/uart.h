/*
 * uart.h - PL011 UART driver for QEMU virt machine
 *
 * The PL011 is ARM's standard UART (serial port) controller.
 * QEMU's 'virt' machine maps it at physical address 0x09000000.
 *
 * How do we know this address?
 *   - QEMU source code: hw/arm/virt.c defines the memory map
 *   - The 'virt' machine's Device Tree also describes it
 *   - ARM PL011 Technical Reference Manual documents the register layout
 *
 * PL011 Register Map (offsets from base 0x09000000):
 *   0x00  UARTDR   - Data Register (read/write characters here)
 *   0x18  UARTFR   - Flag Register (check if TX FIFO is full, etc.)
 *   ...other registers omitted for simplicity...
 *
 * For our simple Hello World, we only need:
 *   - UARTDR: write a byte here to send a character
 *   - UARTFR: check bit 5 (TXFF) to see if the transmit FIFO is full
 */

#ifndef UART_H
#define UART_H

#include <stdint.h>

/* Base address of PL011 UART0 on QEMU 'virt' machine */
#define UART0_BASE  0x09000000

/* Register offsets */
#define UART_DR     0x00    /* Data Register */
#define UART_FR     0x18    /* Flag Register */

/* Flag Register bits */
#define UART_FR_TXFF  (1 << 5)  /* Transmit FIFO Full */

/*
 * uart_putc - Send one character to the serial port
 *
 * This function:
 *   1. Waits until the transmit FIFO has space (TXFF bit is 0)
 *   2. Writes the character to the Data Register
 *
 * 'volatile' tells the compiler: "do not optimize away reads/writes
 * to this address" — because it's a hardware register, not normal memory.
 */
static inline void uart_putc(char c)
{
    volatile uint32_t *uart = (volatile uint32_t *)UART0_BASE;

    /* Spin-wait until transmit FIFO is not full */
    while (uart[UART_FR / 4] & UART_FR_TXFF)
        ;

    /* Write the character */
    uart[UART_DR / 4] = (uint32_t)c;
}

/*
 * uart_puts - Send a null-terminated string to the serial port
 *
 * Converts '\n' to '\r\n' (CRLF) for proper terminal display.
 * Serial terminals expect CR+LF for a new line.
 */
static inline void uart_puts(const char *s)
{
    while (*s) {
        if (*s == '\n')
            uart_putc('\r');
        uart_putc(*s);
        s++;
    }
}

#endif /* UART_H */
