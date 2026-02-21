/*
 * drivers/uart.c — PL011 UART driver
 *
 * The QEMU 'virt' machine exposes a PrimeCell PL011 UART at:
 *   base address: 0x09000000
 *   IRQ:          33 (SPI 1 on GIC, added in Phase 3)
 *
 * This is Phase 1's only output mechanism — no MMU, no interrupts.
 * We use busy-wait polling (UART_FR.TXFF) instead of IRQ-driven TX.
 *
 * PL011 register map (ARM DDI 0183):
 *   +0x000  UARTDR      Data register (TX/RX)
 *   +0x004  UARTRSR     Receive status / error clear
 *   +0x018  UARTFR      Flag register
 *   +0x020  UARTILPR    IrDA low-power counter
 *   +0x024  UARTIBRD    Integer baud rate divisor
 *   +0x028  UARTFBRD    Fractional baud rate divisor
 *   +0x02C  UARTLCR_H   Line control
 *   +0x030  UARTCR      Control register
 *   +0x034  UARTIFLS    FIFO level select
 *   +0x038  UARTIMSC    Interrupt mask set/clear
 *   +0x03C  UARTRIS     Raw interrupt status
 *   +0x040  UARTMIS     Masked interrupt status
 *   +0x044  UARTICR     Interrupt clear
 */

#include "../include/uart.h"

#define UART_BASE       0x09000000UL

#define UART_DR         (*(volatile unsigned int *)(UART_BASE + 0x000))
#define UART_FR         (*(volatile unsigned int *)(UART_BASE + 0x018))
#define UART_IBRD       (*(volatile unsigned int *)(UART_BASE + 0x024))
#define UART_FBRD       (*(volatile unsigned int *)(UART_BASE + 0x028))
#define UART_LCR_H      (*(volatile unsigned int *)(UART_BASE + 0x02C))
#define UART_CR         (*(volatile unsigned int *)(UART_BASE + 0x030))
#define UART_IMSC       (*(volatile unsigned int *)(UART_BASE + 0x038))
#define UART_ICR        (*(volatile unsigned int *)(UART_BASE + 0x044))

/* UARTFR bits */
#define FR_TXFF         (1u << 5)   /* TX FIFO full  — wait before writing */
#define FR_RXFE         (1u << 4)   /* RX FIFO empty */
#define FR_BUSY         (1u << 3)   /* UART transmitting */

/* UARTLCR_H bits */
#define LCR_WLEN8       (3u << 5)   /* 8-bit word length */
#define LCR_FEN         (1u << 4)   /* FIFO enable */

/* UARTCR bits */
#define CR_RXE          (1u << 9)   /* Receive enable */
#define CR_TXE          (1u << 8)   /* Transmit enable */
#define CR_UARTEN       (1u << 0)   /* UART enable */

/*
 * uart_init — configure PL011 for 115200 8N1
 *
 * Baud rate divisor for 115200 bps at UARTCLK = 24 MHz:
 *   BRD = 24000000 / (16 * 115200) = 13.0208...
 *   IBRD = 13
 *   FBRD = round(0.0208 * 64) = 1
 *
 * On QEMU the baud rate is not physically enforced, but setting
 * these registers correctly mirrors real hardware behaviour and
 * is required reading for arch/arm64/kernel/setup.c early console.
 */
void uart_init(void)
{
    /* 1. Disable UART before reconfiguring */
    UART_CR = 0;

    /* 2. Wait for any in-progress transmission to complete */
    while (UART_FR & FR_BUSY)
        ;

    /* 3. Set baud rate */
    UART_IBRD = 13;
    UART_FBRD = 1;

    /* 4. 8 data bits, 1 stop bit, no parity, FIFO enabled */
    UART_LCR_H = LCR_WLEN8 | LCR_FEN;

    /* 5. Mask all interrupts (polled mode for Phase 1) */
    UART_IMSC = 0;

    /* 6. Clear any pending interrupts */
    UART_ICR = 0x7FF;

    /* 7. Enable UART, TX, RX */
    UART_CR = CR_UARTEN | CR_TXE | CR_RXE;
}

/*
 * uart_putc — blocking character output
 *
 * Spins on FR.TXFF (TX FIFO full) before writing to DR.
 * Safe to call before interrupts are enabled.
 */
void uart_putc(char c)
{
    while (UART_FR & FR_TXFF)
        ;
    UART_DR = (unsigned int)c;
}

/*
 * uart_puts — output a null-terminated string
 * Translates '\n' → '\r\n' for serial terminals.
 */
void uart_puts(const char *s)
{
    while (*s) {
        if (*s == '\n')
            uart_putc('\r');
        uart_putc(*s++);
    }
}

/*
 * uart_puthex64 — print a 64-bit value as "0x<16 hex digits>"
 * Useful for printing register values and addresses.
 */
void uart_puthex64(unsigned long n)
{
    static const char hex[] = "0123456789abcdef";
    int i;

    uart_puts("0x");
    for (i = 60; i >= 0; i -= 4)
        uart_putc(hex[(n >> i) & 0xf]);
}

/*
 * uart_putdec — print an unsigned decimal number
 */
void uart_putdec(unsigned long n)
{
    char buf[20];
    int i = 0;

    if (n == 0) {
        uart_putc('0');
        return;
    }
    while (n) {
        buf[i++] = '0' + (n % 10);
        n /= 10;
    }
    while (i--)
        uart_putc(buf[i]);
}
