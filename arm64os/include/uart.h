#ifndef _UART_H
#define _UART_H

void uart_init(void);
void uart_putc(char c);
void uart_puts(const char *s);
void uart_puthex64(unsigned long n);
void uart_putdec(unsigned long n);

#endif /* _UART_H */
