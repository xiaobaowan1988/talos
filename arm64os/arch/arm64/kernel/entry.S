/* SPDX-License-Identifier: GPL-2.0 */
/*
 * arm64os/arch/arm64/kernel/entry.S
 *
 * ARM64 异常向量表与上下文保存/恢复
 *
 * 参考：arch/arm64/kernel/entry.S
 *
 * ARMv8-A 异常向量表结构：
 *   - 4 种来源 × 4 种类型 = 16 个入口
 *   - 每个入口占 128 字节（.align 7 = 2^7）
 *   - 向量表整体需 2KB 对齐（.align 11 = 2^11）
 *
 * pt_regs 布局（272 字节 = 34 × 8）：
 *   [  0-232]: x0-x29（30 个 64-bit 寄存器，15 对 stp）
 *   [240    ]: x30 / LR
 *   [248    ]: SP（EL0，用户态异常时的栈指针，Phase 1 暂不保存）
 *   [256    ]: PC（ELR_EL1，异常返回地址）
 *   [264    ]: PSTATE（SPSR_EL1，保存的处理器状态）
 */

/* ---- pt_regs 偏移常量 ---- */
#define PT_REGS_SIZE    272         /* 34 × 8，16字节对齐（272 = 17 × 16）*/
#define PT_LR           (30 * 8)   /* 240: x30 / Link Register */
#define PT_SP           (31 * 8)   /* 248: 用户态 SP（Phase 1 暂未使用）*/
#define PT_PC           (32 * 8)   /* 256: 异常返回地址（ELR_EL1）*/
#define PT_PSTATE       (33 * 8)   /* 264: 保存的处理器状态（SPSR_EL1）*/

/*
 * kernel_entry 宏
 *
 * 在栈上分配 pt_regs 结构体，保存所有通用寄存器和关键系统寄存器。
 * 调用后 sp 指向 pt_regs 结构体底部（低地址端）。
 *
 * 注意：x21, x22 在此宏中被临时征用存储 ELR/SPSR，
 *       但随即被保存到 pt_regs 中，不影响调用者的 x21/x22。
 */
.macro  kernel_entry
    sub     sp, sp, #PT_REGS_SIZE

    stp     x0,  x1,  [sp, #16 * 0]    /* offset   0,   8 */
    stp     x2,  x3,  [sp, #16 * 1]    /* offset  16,  24 */
    stp     x4,  x5,  [sp, #16 * 2]    /* offset  32,  40 */
    stp     x6,  x7,  [sp, #16 * 3]    /* offset  48,  56 */
    stp     x8,  x9,  [sp, #16 * 4]    /* offset  64,  72 */
    stp     x10, x11, [sp, #16 * 5]    /* offset  80,  88 */
    stp     x12, x13, [sp, #16 * 6]    /* offset  96, 104 */
    stp     x14, x15, [sp, #16 * 7]    /* offset 112, 120 */
    stp     x16, x17, [sp, #16 * 8]    /* offset 128, 136 */
    stp     x18, x19, [sp, #16 * 9]    /* offset 144, 152 */
    stp     x20, x21, [sp, #16 * 10]   /* offset 160, 168 */
    stp     x22, x23, [sp, #16 * 11]   /* offset 176, 184 */
    stp     x24, x25, [sp, #16 * 12]   /* offset 192, 200 */
    stp     x26, x27, [sp, #16 * 13]   /* offset 208, 216 */
    stp     x28, x29, [sp, #16 * 14]   /* offset 224, 232 */
    str     x30,      [sp, #PT_LR]     /* offset 240: LR */

    /* 读取并保存 ELR_EL1（异常发生时的 PC）和 SPSR_EL1（异常发生时的 PSTATE）*/
    mrs     x21, elr_el1
    mrs     x22, spsr_el1
    stp     x21, x22, [sp, #PT_PC]     /* offset 256: PC, offset 264: PSTATE */
.endm

/*
 * kernel_exit 宏
 *
 * 从 pt_regs 恢复所有寄存器，写回 ELR_EL1 / SPSR_EL1，
 * 然后 eret 返回异常发生前的现场。
 */
.macro  kernel_exit
    ldp     x21, x22, [sp, #PT_PC]     /* 读回 PC 和 PSTATE */
    msr     elr_el1,  x21
    msr     spsr_el1, x22

    ldp     x0,  x1,  [sp, #16 * 0]
    ldp     x2,  x3,  [sp, #16 * 1]
    ldp     x4,  x5,  [sp, #16 * 2]
    ldp     x6,  x7,  [sp, #16 * 3]
    ldp     x8,  x9,  [sp, #16 * 4]
    ldp     x10, x11, [sp, #16 * 5]
    ldp     x12, x13, [sp, #16 * 6]
    ldp     x14, x15, [sp, #16 * 7]
    ldp     x16, x17, [sp, #16 * 8]
    ldp     x18, x19, [sp, #16 * 9]
    ldp     x20, x21, [sp, #16 * 10]
    ldp     x22, x23, [sp, #16 * 11]
    ldp     x24, x25, [sp, #16 * 12]
    ldp     x26, x27, [sp, #16 * 13]
    ldp     x28, x29, [sp, #16 * 14]
    ldr     x30,      [sp, #PT_LR]

    add     sp, sp, #PT_REGS_SIZE
    eret
.endm

/*
 * ventry 宏：生成一个向量入口（128字节）
 *
 * .align 7 = 128 字节对齐，满足 ARMv8 向量表每入口 128 字节的要求。
 * 只有一条 b 指令（4字节），其余空间由对齐填充。
 */
.macro  ventry  label
    .align  7
    b       \label
.endm

/*
 * =============================================================
 * 异常向量表
 *
 * 必须 2KB 对齐（.align 11 = 2^11 = 2048）。
 * VBAR_EL1 寄存器须指向此地址（在 head.S 的 setup_el1 中设置）。
 * =============================================================
 */
    .section ".text", "ax"

    .align  11
    .global vectors
vectors:
    /*
     * Group 1: 当前 EL，使用 SP_EL0（正常内核代码不使用 SP_EL0，
     *           到达这里意味着严重错误）
     */
    ventry  el1t_sync               /* 同步异常（data/instruction abort, SVC 等）*/
    ventry  el1t_irq                /* IRQ */
    ventry  el1t_fiq                /* FIQ */
    ventry  el1t_error              /* SError（异步错误）*/

    /*
     * Group 2: 当前 EL，使用 SP_EL1（内核态的正常中断/异常路径）
     */
    ventry  el1h_sync               /* 内核态同步异常（Phase 3+ 处理 data abort 等）*/
    ventry  el1h_irq                /* 内核态 IRQ（Phase 3 GIC 初始化后使用）*/
    ventry  el1h_fiq                /* 内核态 FIQ（通常不使用）*/
    ventry  el1h_error              /* 内核态 SError（不可恢复错误）*/

    /*
     * Group 3: 低 EL（用户态），AArch64
     * Phase 5 将完善系统调用（SVC）处理。
     */
    ventry  el0_sync                /* 用户态同步异常，SVC（系统调用）走这里 */
    ventry  el0_irq                 /* 用户态 IRQ */
    ventry  el0_fiq                 /* 用户态 FIQ */
    ventry  el0_error               /* 用户态 SError */

    /*
     * Group 4: 低 EL（用户态），AArch32
     * 我们不支持 AArch32，直接 panic。
     */
    ventry  el0_32_sync
    ventry  el0_32_irq
    ventry  el0_32_fiq
    ventry  el0_32_error

/*
 * =============================================================
 * 异常处理函数实现
 * =============================================================
 */

/*
 * el1t_*: 当前 EL（EL1），SP_EL0 — 异常配置错误，不应发生
 */
el1t_sync:
el1t_irq:
el1t_fiq:
el1t_error:
    kernel_entry
    bl      panic_unhandled
    b       .                       /* 不应返回，此处挂死以防万一 */

/*
 * el1h_sync: 内核态同步异常
 *
 * 常见原因：data abort, instruction abort, PC alignment fault。
 * Phase 1：打印错误信息后挂死（后续 Phase 完善处理）。
 * Phase 2+ 将在此处处理缺页异常（page fault）。
 */
el1h_sync:
    kernel_entry
    mov     x0, sp                  /* 传入 pt_regs 指针作为第一个参数 */
    bl      handle_sync_exception
    kernel_exit

/*
 * el1h_irq: 内核态 IRQ
 *
 * Phase 1：简单打印（此时 GIC 未初始化，不会有真实 IRQ）。
 * Phase 3 GIC v3 初始化后将在此调用中断处理框架。
 */
el1h_irq:
    kernel_entry
    mov     x0, sp
    bl      handle_irq
    kernel_exit

/*
 * el1h_fiq: 内核态 FIQ — 通常不使用，视为不可恢复错误
 */
el1h_fiq:
    kernel_entry
    bl      panic_unhandled
    b       .

/*
 * el1h_error: 内核态 SError（异步中止）— 不可恢复
 */
el1h_error:
    kernel_entry
    bl      panic_unhandled
    b       .

/*
 * el0_sync: 用户态同步异常
 *
 * SVC 指令（系统调用）、用户态缺页等均从这里进入。
 * Phase 5 将完整实现系统调用分发。
 * Phase 1：简单转发给 C 处理函数。
 */
el0_sync:
    kernel_entry
    mov     x0, sp
    bl      handle_sync_exception
    kernel_exit

/*
 * el0_irq: 用户态 IRQ
 */
el0_irq:
    kernel_entry
    mov     x0, sp
    bl      handle_irq
    kernel_exit

/*
 * el0_fiq/error: 用户态 FIQ / SError — 不支持
 */
el0_fiq:
el0_error:
    kernel_entry
    bl      panic_unhandled
    b       .

/*
 * el0_32_*: AArch32 用户态异常 — 不支持
 */
el0_32_sync:
el0_32_irq:
el0_32_fiq:
el0_32_error:
    kernel_entry
    bl      panic_unhandled
    b       .
