/* SPDX-License-Identifier: GPL-2.0 */
/*
 * arm64os/include/linux/irq.h
 *
 * IRQ 子系统头文件
 *
 * 参考：include/linux/irq.h, include/linux/interrupt.h
 *
 * Phase 3 简化实现：
 *   - 固定大小的 irq_handler_t 函数指针数组（1020项）
 *   - request_irq() 注册中断处理函数
 *   - handle_irq() 由 entry.S 调用，通过 GIC IAR 分发中断
 */

#ifndef __LINUX_IRQ_H
#define __LINUX_IRQ_H

/*
 * NR_IRQS：GIC v3 支持的最大中断数
 * SPI 上限为 IRQ 1019，保留 1020-1023 为特殊值（spurious 等）
 */
#define NR_IRQS     1020

/*
 * irq_handler_t：中断处理函数类型
 *
 * Phase 3 使用无参数无返回值的简单函数指针。
 * Linux 内核实际使用 irqreturn_t (*handler)(int, void *)。
 */
typedef void (*irq_handler_t)(void);

/*
 * irq_handlers[]：IRQ 处理函数表
 *
 * 定义在 kernel/irq/irqdesc.c，按 IRQ 号索引。
 * 未注册的项为 NULL。
 *
 * IRQ 布局（GIC v3）：
 *   [0-15]  : SGI（核间中断，Phase 3 暂不使用）
 *   [16-31] : PPI（每CPU私有中断，#27 = Virtual Timer）
 *   [32-1019]: SPI（共享外设中断，Phase 6 VirtIO 使用）
 */
extern irq_handler_t irq_handlers[NR_IRQS];

/*
 * request_irq - 注册中断处理函数
 *
 * @irq:     中断号（0-1019）
 * @handler: 处理函数指针（NULL 表示注销）
 *
 * 参考：include/linux/interrupt.h: request_irq()
 */
void request_irq(unsigned int irq, irq_handler_t handler);

#endif /* __LINUX_IRQ_H */
