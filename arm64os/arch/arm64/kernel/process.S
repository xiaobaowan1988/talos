/* SPDX-License-Identifier: GPL-2.0 */
/*
 * arm64os/arch/arm64/kernel/process.S
 *
 * ARM64 上下文切换汇编
 *
 * 参考：arch/arm64/kernel/entry.S（ret_from_fork）
 *       arch/arm64/kernel/process.c（cpu_switch_to）
 *
 * Phase 4 实现：
 *   cpu_switch_to(prev, next)：保存 prev 的 callee-saved 寄存器，
 *   恢复 next 的寄存器，跳转到 next 的恢复点。
 *
 * struct cpu_context 布局（来自 include/linux/sched.h）：
 *   offset  0: x19
 *   offset  8: x20
 *   offset 16: x21
 *   offset 24: x22
 *   offset 32: x23
 *   offset 40: x24
 *   offset 48: x25
 *   offset 56: x26
 *   offset 64: x27
 *   offset 72: x28
 *   offset 80: fp  (x29)
 *   offset 88: sp
 *   offset 96: pc  (返回地址)
 *
 * THREAD_CPU_CONTEXT = offsetof(struct task_struct, thread) = 112
 *   计算：state(8) + flags(8) + stack(8) + pid(4) + prio(4) + comm(16) +
 *         sched_entity(64) = 112
 *   sched_entity = load_weight(8) + rb_node(24) + on_rq(4) + pad(4) +
 *                  vruntime(8) + exec_start(8) + sum_exec_runtime(8) = 64
 */

/*
 * THREAD_CPU_CONTEXT = offsetof(struct task_struct, thread)
 *
 * 该值必须与 include/linux/sched.h 中 struct task_struct 的布局一致。
 * 当 task_struct 的成员变化时，必须同步更新此值。
 *
 * 参考 include/linux/sched.h 中的 #define THREAD_CPU_CONTEXT 112
 */
#define THREAD_CPU_CONTEXT  112

    .section ".text", "ax"

/*
 * cpu_switch_to - 上下文切换
 *
 * 参数：
 *   x0 = prev task_struct 指针
 *   x1 = next task_struct 指针
 *
 * 返回：
 *   当 prev 被再次选中执行时，从 ret 指令返回，x0 = prev
 *
 * callee-saved 寄存器（AArch64 PCS）：x19-x28, x29(FP), SP, LR(x30)
 * 我们保存 x19-x28, FP, SP, LR 到 prev->thread（struct cpu_context）。
 *
 * Phase 4：不切换页表（所有进程为内核线程，共享恒等映射）。
 */
    .global cpu_switch_to
cpu_switch_to:
    /* 计算 prev->thread 的地址 */
    mov     x10, #THREAD_CPU_CONTEXT
    add     x8, x0, x10

    /* 保存 callee-saved 寄存器到 prev->thread */
    stp     x19, x20, [x8, #0]     /* offset 0, 8 */
    stp     x21, x22, [x8, #16]    /* offset 16, 24 */
    stp     x23, x24, [x8, #32]    /* offset 32, 40 */
    stp     x25, x26, [x8, #48]    /* offset 48, 56 */
    stp     x27, x28, [x8, #64]    /* offset 64, 72 */

    /* 保存 FP 和 SP */
    mov     x9, sp
    stp     x29, x9,  [x8, #80]    /* offset 80(fp), 88(sp) */

    /* 保存返回地址（LR）作为恢复时的 PC */
    str     x30,      [x8, #96]    /* offset 96(pc) */

    /* ---- 恢复 next 的上下文 ---- */

    /* 计算 next->thread 的地址 */
    mov     x10, #THREAD_CPU_CONTEXT
    add     x8, x1, x10

    /* 恢复 callee-saved 寄存器 */
    ldp     x19, x20, [x8, #0]
    ldp     x21, x22, [x8, #16]
    ldp     x23, x24, [x8, #32]
    ldp     x25, x26, [x8, #48]
    ldp     x27, x28, [x8, #64]

    /* 恢复 FP 和 SP */
    ldp     x29, x9,  [x8, #80]    /* fp, sp */
    mov     sp, x9

    /* 恢复 PC（返回地址）*/
    ldr     x9,       [x8, #96]    /* pc */

    /*
     * 跳转到 next 的恢复点：
     * - 如果 next 之前通过 cpu_switch_to 切出，x9 = 该处的 LR
     *   → 返回到 context_switch() → schedule() 调用者
     * - 如果 next 是新创建的线程，x9 = ret_from_kernel_thread
     *   → 跳转到 ret_from_kernel_thread()
     */
    br      x9
