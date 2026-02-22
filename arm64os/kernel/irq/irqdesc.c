/* SPDX-License-Identifier: GPL-2.0 */
/*
 * arm64os/kernel/irq/irqdesc.c
 *
 * IRQ 描述符管理
 *
 * 参考：kernel/irq/irqdesc.c, kernel/irq/manage.c
 *
 * Phase 3 简化实现：
 *   - irq_handlers[]：IRQ 处理函数指针数组，按 IRQ 号索引
 *   - request_irq()：注册/注销处理函数
 *
 * Linux 内核的 struct irq_desc 包含锁、统计计数、设备信息等字段。
 * 这里仅保留最核心的函数指针，足以驱动 GIC v3 + arch timer。
 */

#include <linux/types.h>
#include <linux/irq.h>

/*
 * irq_handlers[] - 中断处理函数表
 *
 * 全局数组，初始化为全 NULL。
 * 由 request_irq() 填充，由 handle_irq() 读取分发。
 *
 * 参考：Linux 内核通过 irq_desc[NR_IRQS] 数组管理每个 IRQ 的描述符；
 *       此处将其简化为单一函数指针数组。
 */
irq_handler_t irq_handlers[NR_IRQS];

/*
 * request_irq - 注册中断处理函数
 *
 * @irq:     中断号（0 .. NR_IRQS-1）
 * @handler: 处理函数（NULL 表示注销）
 *
 * 参考：kernel/irq/manage.c: __setup_irq()
 */
void request_irq(unsigned int irq, irq_handler_t handler)
{
    if (irq >= NR_IRQS)
        return;

    irq_handlers[irq] = handler;
}
