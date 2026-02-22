/* SPDX-License-Identifier: GPL-2.0 */
/*
 * arm64os/kernel/irq/handle.c
 *
 * 中断处理框架
 *
 * 参考：kernel/irq/handle.c, arch/arm64/kernel/irq.c
 *
 * 此文件实现 handle_irq()，由 entry.S 中的 el1h_irq / el0_irq 调用。
 *
 * 处理流程：
 *   1. 通过 ICC_IAR1_EL1 读取中断号（Interrupt Acknowledge Register）
 *      — 读取此寄存器同时将中断标记为 Active 状态
 *   2. 根据中断号分发到 irq_handlers[] 中注册的处理函数
 *   3. 通过 ICC_EOIR1_EL1 写入 End-of-Interrupt（将中断标记为 Inactive）
 *
 * 中断号特殊值（GIC 规范）：
 *   1020 = spurious interrupt（无中断，硬件返回的哨兵值）
 *   1021-1023 = reserved
 */

#include <linux/types.h>
#include <linux/irq.h>

/*
 * pt_regs 前向声明（定义在 kernel/main.c）
 * entry.S 调用时通过 x0 传入 sp（即 pt_regs 指针）。
 */
struct pt_regs;

/*
 * handle_irq - 中断分发入口
 *
 * @regs: 指向栈上 pt_regs 结构体的指针（由 kernel_entry 宏构建）
 *
 * 参考：arch/arm64/kernel/irq.c: handle_arch_irq()
 *       kernel/irq/handle.c: handle_irq_event_percpu()
 */
void handle_irq(struct pt_regs *regs)
{
    u64 irqnr;

    (void)regs;

    /*
     * 读取 Interrupt Acknowledge Register（Group 1）。
     * 此操作同时将该中断状态从 Pending 变为 Active。
     * 低 10 位为中断号，高位保留。
     *
     * 参考：ARM GIC Architecture Specification,
     *       Section 4.8.12: ICC_IAR1_EL1
     */
    __asm__ volatile("mrs %0, ICC_IAR1_EL1" : "=r"(irqnr));
    irqnr &= 0x3FFU;  /* 取低10位：有效中断号范围 0-1019 */

    /*
     * 分发中断。
     * IRQ 1020-1023 为特殊值（spurious/no interrupt），跳过处理但仍需 EOI。
     */
    if (irqnr < NR_IRQS && irq_handlers[irqnr])
        irq_handlers[irqnr]();

    /*
     * End of Interrupt：通知 GIC CPU 接口已完成对该中断的处理。
     * 将该中断状态从 Active 变回 Inactive（或 Pending，若中断再次触发）。
     *
     * 参考：ARM GIC Architecture Specification,
     *       Section 4.8.14: ICC_EOIR1_EL1
     */
    __asm__ volatile("msr ICC_EOIR1_EL1, %0" :: "r"(irqnr));
    __asm__ volatile("isb");
}
