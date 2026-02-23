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
 * Phase 5 更新：
 *   THREAD_CPU_CONTEXT 从 112 → 120（Phase 5：新增 mm 字段）→ 128（Phase 7：新增 files 字段）
 *   → 144（Phase 10：新增 nsproxy + cgroups 字段，各 8 字节）
 *   → 168（Phase 12：新增 seccomp_mode + seccomp_filter + landlock_domain 字段）
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
 * THREAD_CPU_CONTEXT = offsetof(struct task_struct, thread) = 168
 *   计算（Phase 12）：state(8) + flags(8) + stack(8) + pid(4) + prio(4) +
 *         comm(16) + mm(8) + files(8) + nsproxy(8) + cgroups(8) +
 *         seccomp_mode(4) + pad(4) + seccomp_filter(8) + landlock_domain(8) +
 *         sched_entity(64) = 168
 */

/*
 * THREAD_CPU_CONTEXT = offsetof(struct task_struct, thread)
 * Phase 12 更新：从 144 → 168（新增 seccomp_mode(4) + pad(4) + seccomp_filter(8) + landlock_domain(8) = 24 字节）
 */
#define THREAD_CPU_CONTEXT  168

/*
 * TASK_MM_OFFSET = offsetof(struct task_struct, mm)
 *   state(8) + flags(8) + stack(8) + pid(4) + prio(4) + comm(16) = 48
 */
#define TASK_MM_OFFSET      48

/*
 * MM_PGD_OFFSET = offsetof(struct mm_struct, pgd) = 0
 */
#define MM_PGD_OFFSET       0

    .section ".text", "ax"

/*
 * cpu_switch_to - 上下文切换
 *
 * 参数：
 *   x0 = prev task_struct 指针
 *   x1 = next task_struct 指针
 *
 * Phase 5 新增：检查 next->mm，如果非 NULL 则切换 TTBR0_EL1。
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

    /* ---- 切换 TTBR0（Phase 5）---- */
    /*
     * 检查 next->mm：
     *   如果 next->mm != NULL → 用户进程，切换 TTBR0 到 next->mm->pgd
     *   如果 next->mm == NULL → 内核线程，切换回内核 init_pgd
     *
     * 由 C 层的 context_switch 处理（通过 switch_mm 调用）。
     * 此处不直接在汇编中实现，保持与 Phase 4 相同的简洁接口。
     * TTBR0 切换由 C 层在调用 cpu_switch_to 之前完成。
     */

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
     * - 如果 next 是新创建的内核线程，x9 = ret_from_kernel_thread
     * - 如果 next 是新创建的用户进程，x9 = ret_to_user
     *   → 跳转到 ret_to_user → kernel_exit 0 → eret 到 EL0
     */
    br      x9
