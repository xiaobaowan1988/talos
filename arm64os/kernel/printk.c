/* SPDX-License-Identifier: GPL-2.0 */
/*
 * arm64os/kernel/printk.c
 *
 * 早期串口调试输出（基于 PL011 UART）
 *
 * QEMU virt machine 的 PL011 UART 物理地址：0x09000000
 * 对应设备：ARM PrimeCell UART (PL011)
 *
 * Phase 1 不初始化 UART（QEMU 已默认开启），只做最简单的字符输出。
 * Phase 3+ 将完善 UART 驱动，支持中断驱动输出和 /dev/ttyS0。
 *
 * 参考：drivers/tty/serial/amba-pl011.c
 */

#include <linux/types.h>

/*
 * PL011 UART 物理基址（QEMU virt machine）
 * 参考：QEMU hw/arm/virt.c, MachineClass virt_map[] 中的 UART 节点
 */
#define PL011_BASE      0x09000000UL

/*
 * PL011 寄存器偏移（字节）
 * 参考：ARM PrimeCell UART (PL011) Technical Reference Manual
 */
#define UARTDR          0x000       /* 数据寄存器：写入=发送，读出=接收 */
#define UARTFR          0x018       /* 标志寄存器（Flag Register）*/
#define UARTIBRD        0x024       /* 整数波特率寄存器 */
#define UARTFBRD        0x028       /* 小数波特率寄存器 */
#define UARTLCR_H       0x02c       /* 线控寄存器（Line Control）*/
#define UARTCR          0x030       /* 控制寄存器（Control Register）*/
#define UARTIMSC        0x038       /* 中断屏蔽寄存器 */

/*
 * UARTFR 标志位
 */
#define UARTFR_TXFF     (1 << 5)   /* 发送 FIFO 满（Transmit FIFO Full）*/
#define UARTFR_RXFE     (1 << 4)   /* 接收 FIFO 空（Receive FIFO Empty）*/
#define UARTFR_BUSY     (1 << 3)   /* UART 忙（正在发送）*/

/*
 * MMIO 寄存器访问辅助宏
 * volatile 确保编译器不优化掉硬件寄存器读写。
 */
#define PL011_REG(offset) \
    (*((volatile unsigned int *)(PL011_BASE + (offset))))

/*
 * uart_putchar - 输出单个字符到 PL011 UART
 *
 * 等待发送 FIFO 有空位后写入字符。
 * QEMU 的 FIFO 通常很快，等待时间极短。
 */
static void uart_putchar(char c)
{
    /* 轮询等待：TXFF（发送FIFO满）清零后才能写入 */
    while (PL011_REG(UARTFR) & UARTFR_TXFF)
        ;

    PL011_REG(UARTDR) = (unsigned int)(unsigned char)c;
}

/*
 * uart_puts - 输出以 '\0' 结尾的字符串到 UART
 *
 * '\n' 前自动补发 '\r' 以兼容串口终端（CR+LF）。
 */
static void uart_puts(const char *s)
{
    while (*s) {
        if (*s == '\n')
            uart_putchar('\r');
        uart_putchar(*s++);
    }
}

/*
 * boot_printk - 内核早期调试输出接口
 *
 * Phase 1 版本：直接写 PL011 UART，无锁无缓冲。
 * 供 main.c 和异常处理函数调用。
 *
 * 使用示例：
 *   boot_printk("[BOOT] ARM64 kernel starting...\n");
 */
void boot_printk(const char *s)
{
    uart_puts(s);
}

/*
 * boot_printk_char - 输出单个字符到 UART
 *
 * Phase 5 新增：供 sys_write 逐字符输出用户缓冲区。
 * '\n' 前自动补发 '\r' 以兼容串口终端。
 */
void boot_printk_char(char c)
{
    if (c == '\n')
        uart_putchar('\r');
    uart_putchar(c);
}

/*
 * boot_printk_hex - 输出 64-bit 十六进制值（调试用）
 *
 * 格式：0xXXXXXXXXXXXXXXXX
 */
void boot_printk_hex(unsigned long val)
{
    static const char hex[] = "0123456789abcdef";
    char buf[19];               /* "0x" + 16 hex digits + '\0' */
    int i;

    buf[0] = '0';
    buf[1] = 'x';
    for (i = 0; i < 16; i++)
        buf[2 + i] = hex[(val >> (60 - i * 4)) & 0xf];
    buf[18] = '\0';

    uart_puts(buf);
}
