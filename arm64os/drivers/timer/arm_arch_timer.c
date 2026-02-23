/* SPDX-License-Identifier: GPL-2.0 */
/*
 * arm64os/drivers/timer/arm_arch_timer.c
 *
 * ARMv8 通用计时器驱动
 *
 * 参考：drivers/clocksource/arm_arch_timer.c
 *       include/clocksource/arm_arch_timer.h
 *       ARMv8-A Architecture Reference Manual, Section D13.8（Generic Timer）
 *
 * ARMv8 每 CPU 提供多个计时器，本阶段使用 EL1 Virtual Timer：
 *   CNTV_CTL_EL0  — 控制寄存器（ENABLE/IMASK/ISTATUS）
 *   CNTV_TVAL_EL0 — TimerValue（写入倒计时 tick 数）
 *   CNTFRQ_EL0    — 计数频率（QEMU virt 默认 62.5 MHz = 62500000 Hz）
 *
 * Virtual Timer PPI 中断号：#27（IRQ 27 in GIC terms）
 *
 * Phase 3 实现：
 *   - arch_timer_init()：注册 PPI #27 处理函数，使能 PPI，启动计时
 *   - arch_timer_handler()：每 10ms 触发一次，递增 arch_timer_tick_count
 *   - arch_timer_set_next_event_ns()：设置下次中断时间（纳秒）
 *   - arch_timer_tick_count：全局 tick 计数，由 start_kernel() 轮询
 *
 * Phase 4 新增：
 *   - arch_timer_handler() 调用 scheduler_tick() + schedule()，驱动 CFS 调度
 */

#include <linux/types.h>
#include <linux/irq.h>
#include <linux/sched.h>

/* ============================================================
 * 计时器控制寄存器位定义（CNTV_CTL_EL0）
 * 参考：ARMv8-A ARM, Section D13.8.3
 * ============================================================ */
#define ARCH_TIMER_CTL_ENABLE   (1U << 0)  /* 1=计时器运行，0=停止 */
#define ARCH_TIMER_CTL_IMASK    (1U << 1)  /* 1=屏蔽中断输出（计时器仍运行）*/
#define ARCH_TIMER_CTL_ISTATUS  (1U << 2)  /* 只读：1=计时器已到期 */

/* 外部依赖：GIC PPI 使能（drivers/irqchip/gic-v3.c）*/
void gicv3_enable_ppi(unsigned int irq);

/*
 * arch_timer_tick_count - 全局 tick 计数器
 *
 * volatile 确保编译器每次从内存读取，而非从寄存器缓存，
 * 因为该变量在中断上下文（arch_timer_handler）和主线程（start_kernel）
 * 之间共享。
 *
 * 参考：Linux 内核 jiffies 全局计数器的类似使用场景。
 */
volatile int arch_timer_tick_count = 0;

/* ============================================================
 * arch_timer_get_cntfrq - 读取计时器频率
 *
 * 返回 CNTFRQ_EL0，QEMU virt 通常为 62500000（62.5 MHz）。
 * 参考：arm_arch_timer.c: arch_timer_get_cntfrq()
 * ============================================================ */
static u64 arch_timer_get_cntfrq(void)
{
    u64 val;
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(val));
    return val;
}

/* ============================================================
 * arch_timer_set_next_event_ns - 设置 Virtual Timer 下次触发时间
 *
 * @ns: 距现在的纳秒数
 *
 * 写入 CNTV_TVAL_EL0：从当前计数器值开始，经过 ticks 个计数单位后触发。
 * 使能计时器并清除 IMASK，允许中断输出。
 *
 * ticks = ns * cntfrq / 1e9
 *
 * 参考：arm_arch_timer.c: arch_timer_set_next_event_virt()
 * ============================================================ */
void arch_timer_set_next_event_ns(u64 ns)
{
    u64 freq = arch_timer_get_cntfrq();
    u64 ticks = (ns * freq) / 1000000000ULL;

    /* 写入倒计时值（写入即生效）*/
    __asm__ volatile("msr cntv_tval_el0, %0" :: "r"(ticks));
    /* 使能计时器，清除 IMASK，允许中断输出 */
    __asm__ volatile("msr cntv_ctl_el0, %0" ::
                     "r"((u64)ARCH_TIMER_CTL_ENABLE));
    __asm__ volatile("isb");
}

/* ============================================================
 * arch_timer_handler - Virtual Timer PPI #27 中断处理函数
 *
 * 由 handle_irq() 通过 irq_handlers[27] 调用。
 *
 * 处理流程：
 *   1. 屏蔽计时器中断（避免在重新装载期间重复触发）
 *   2. 递增全局 tick 计数
 *   3. 重新设置下次中断（HZ=100，10ms）
 *
 * 参考：arm_arch_timer.c: arch_timer_handler_virt()
 *       kernel/time/tick-common.c: tick_handle_periodic()
 * ============================================================ */
static void arch_timer_handler(void)
{
    /*
     * 先屏蔽计时器中断输出（设置 IMASK，保留 ENABLE）。
     * 这防止在重新装载 TVAL 之前计时器再次触发中断。
     */
    __asm__ volatile("msr cntv_ctl_el0, %0" ::
                     "r"((u64)(ARCH_TIMER_CTL_ENABLE | ARCH_TIMER_CTL_IMASK)));

    /* 递增 tick 计数（start_kernel 轮询此值验证计时器工作）*/
    arch_timer_tick_count++;

    /*
     * 触发调度器 tick：更新当前进程 vruntime，判断是否需要抢占。
     *
     * scheduler_tick() 仅更新 vruntime 和设置 TIF_NEED_RESCHED 标志。
     * 实际的上下文切换不在中断上下文中进行，而是：
     *   - 内核线程主动调用 schedule()（协作式）
     *   - 中断返回路径检查标志（Phase 5 完善）
     *
     * Phase 4 使用协作式调度：线程定期调用 schedule() 检查标志。
     *
     * 参考：kernel/time/tick-common.c tick_handle_periodic()
     */
    scheduler_tick();

    /* 重新设置下次中断：10ms（HZ=100）*/
    arch_timer_set_next_event_ns(10000000ULL);
}

/* ============================================================
 * arch_timer_init - 初始化 ARMv8 Virtual Timer
 *
 * 调用时机：start_kernel() 中，gicv3_init() 之后，daifclr 之前。
 *
 * 参考：arm_arch_timer.c: arch_timer_register()
 *       arch/arm64/kernel/time.c: time_init()
 * ============================================================ */
void arch_timer_init(void)
{
    /*
     * 注册 Virtual Timer 中断处理函数（PPI #27）。
     * GIC Virtual Timer → PPI #27 → irq_handlers[27]。
     */
    request_irq(27, arch_timer_handler);

    /*
     * 在 Redistributor 的 SGI 帧中使能 PPI #27。
     * 必须在 GIC 初始化（gicv3_init）之后调用。
     */
    gicv3_enable_ppi(27);

    /*
     * 启动计时器：10ms 后触发第一次中断。
     * daifclr（IRQ 使能）之后，中断才会真正到达 CPU。
     */
    arch_timer_set_next_event_ns(10000000ULL);
}
