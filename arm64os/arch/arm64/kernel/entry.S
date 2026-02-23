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
 *   [248    ]: SP（EL0 用户态栈指针）
 *   [256    ]: PC（ELR_EL1，异常返回地址）
 *   [264    ]: PSTATE（SPSR_EL1，保存的处理器状态）
 *
 * Phase 5 更新：
 *   - kernel_entry 增加 el 参数区分 EL0/EL1 来源
 *   - EL0 来源时保存/恢复 SP_EL0（用户栈指针）
 *   - el0_sync 增加 ESR_EL1.EC 判断：
 *     EC=0x15 → SVC（系统调用） → do_el0_svc()
 *     其他    → handle_sync_exception()
 */

/* ---- pt_regs 偏移常量 ---- */
#define PT_REGS_SIZE    272         /* 34 × 8，16字节对齐（272 = 17 × 16）*/
#define PT_LR           (30 * 8)   /* 240: x30 / Link Register */
#define PT_SP           (31 * 8)   /* 248: 用户态 SP_EL0 */
#define PT_PC           (32 * 8)   /* 256: 异常返回地址（ELR_EL1）*/
#define PT_PSTATE       (33 * 8)   /* 264: 保存的处理器状态（SPSR_EL1）*/

/* ESR_EL1 Exception Class 位域 */
#define ESR_ELx_EC_SHIFT    26
#define ESR_ELx_EC_SVC64    0x15    /* AArch64 SVC 指令 */

/*
 * kernel_entry 宏
 *
 * 在栈上分配 pt_regs 结构体，保存所有通用寄存器和关键系统寄存器。
 * 调用后 sp 指向 pt_regs 结构体底部（低地址端）。
 *
 * 参数：
 *   el = 0: 来自 EL0（用户态），需要保存 SP_EL0
 *   el = 1: 来自 EL1（内核态），不保存 SP_EL0
 */
.macro  kernel_entry, el
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

    /* 读取并保存 ELR_EL1 和 SPSR_EL1 */
    mrs     x21, elr_el1
    mrs     x22, spsr_el1
    stp     x21, x22, [sp, #PT_PC]     /* offset 256: PC, offset 264: PSTATE */

    /*
     * 保存用户态栈指针 SP_EL0（仅 EL0 来源时有意义）。
     */
    .if \el == 0
    mrs     x21, sp_el0
    str     x21, [sp, #PT_SP]          /* offset 248: 用户态 SP */
    .endif
.endm

/*
 * kernel_exit 宏
 *
 * 从 pt_regs 恢复所有寄存器，写回 ELR_EL1 / SPSR_EL1，
 * 然后 eret 返回异常发生前的现场。
 *
 * 参数：
 *   el = 0: 返回 EL0（用户态），需要恢复 SP_EL0
 *   el = 1: 返回 EL1（内核态），不恢复 SP_EL0
 */
.macro  kernel_exit, el
    /* 恢复 SP_EL0（仅返回用户态时）*/
    .if \el == 0
    ldr     x21, [sp, #PT_SP]
    msr     sp_el0, x21
    .endif

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
 */
.macro  ventry  label
    .align  7
    b       \label
.endm

/*
 * =============================================================
 * 异常向量表
 * =============================================================
 */
    .section ".text", "ax"

    .align  11
    .global vectors
vectors:
    /* Group 1: 当前 EL，使用 SP_EL0（配置错误）*/
    ventry  el1t_sync
    ventry  el1t_irq
    ventry  el1t_fiq
    ventry  el1t_error

    /* Group 2: 当前 EL，使用 SP_EL1（内核态正常路径）*/
    ventry  el1h_sync
    ventry  el1h_irq
    ventry  el1h_fiq
    ventry  el1h_error

    /* Group 3: 低 EL（用户态），AArch64 */
    ventry  el0_sync
    ventry  el0_irq
    ventry  el0_fiq
    ventry  el0_error

    /* Group 4: 低 EL（用户态），AArch32（不支持）*/
    ventry  el0_32_sync
    ventry  el0_32_irq
    ventry  el0_32_fiq
    ventry  el0_32_error

/*
 * =============================================================
 * 异常处理函数实现
 * =============================================================
 */

/* el1t_*: 当前 EL（EL1），SP_EL0 — 异常配置错误 */
el1t_sync:
el1t_irq:
el1t_fiq:
el1t_error:
    kernel_entry 1
    bl      panic_unhandled
    b       .

/* el1h_sync: 内核态同步异常 */
el1h_sync:
    kernel_entry 1
    mov     x0, sp
    bl      handle_sync_exception
    kernel_exit 1

/* el1h_irq: 内核态 IRQ */
el1h_irq:
    kernel_entry 1
    mov     x0, sp
    bl      handle_irq
    kernel_exit 1

/* el1h_fiq: 内核态 FIQ — 不可恢复 */
el1h_fiq:
    kernel_entry 1
    bl      panic_unhandled
    b       .

/* el1h_error: 内核态 SError — 不可恢复 */
el1h_error:
    kernel_entry 1
    bl      panic_unhandled
    b       .

/*
 * el0_sync: 用户态同步异常
 *
 * Phase 5：检查 ESR_EL1.EC 字段区分异常类型：
 *   EC=0x15 → SVC（系统调用）→ 调用 do_el0_svc(pt_regs)
 *   其他    → handle_sync_exception(pt_regs)
 *
 * 参考：arch/arm64/kernel/entry.S el0t_64_sync_handler
 */
el0_sync:
    kernel_entry 0

    /* 读取 ESR_EL1.EC 字段 */
    mrs     x25, esr_el1
    lsr     x24, x25, #ESR_ELx_EC_SHIFT
    and     x24, x24, #0x3f

    /* 判断是否为 SVC（系统调用）*/
    cmp     x24, #ESR_ELx_EC_SVC64
    b.eq    el0_svc

    /* 非 SVC：转发给通用同步异常处理 */
    mov     x0, sp
    bl      handle_sync_exception
    kernel_exit 0

/*
 * el0_svc: 用户态系统调用入口
 *
 * 传 pt_regs 指针给 C 层的 do_el0_svc()，由其读取 x8（syscall number）
 * 并分发到具体的 sys_xxx 处理函数。
 *
 * 返回时 pt_regs.regs[0] 已被设为系统调用返回值。
 * kernel_exit 0 会恢复 SP_EL0 并 eret 回用户态。
 *
 * 参考：arch/arm64/kernel/syscall.c invoke_syscall
 */
el0_svc:
    /* 使能中断：系统调用处理期间可被 timer tick 抢占 */
    msr     daifclr, #2

    mov     x0, sp              /* x0 = pt_regs 指针 */
    bl      do_el0_svc          /* C 层系统调用分发 */

    /* 关闭中断：准备返回用户态 */
    msr     daifset, #2

    kernel_exit 0

/* el0_irq: 用户态 IRQ */
el0_irq:
    kernel_entry 0
    mov     x0, sp
    bl      handle_irq
    kernel_exit 0

/* el0_fiq/error: 用户态 FIQ / SError — 不支持 */
el0_fiq:
el0_error:
    kernel_entry 0
    bl      panic_unhandled
    b       .

/* el0_32_*: AArch32 用户态异常 — 不支持 */
el0_32_sync:
el0_32_irq:
el0_32_fiq:
el0_32_error:
    kernel_entry 1
    bl      panic_unhandled
    b       .

/*
 * =============================================================
 * ret_to_user - 从内核态首次进入用户态
 *
 * Phase 5：用于新创建的用户进程首次调度执行时。
 * 假设内核栈顶已放好 pt_regs（由 create_user_task 设置），
 * 包含用户态 PC、SP、PSTATE。
 *
 * cpu_switch_to 跳转到此处时，sp 指向 pt_regs。
 *
 * 参考：arch/arm64/kernel/entry.S ret_from_fork
 * =============================================================
 */
    .global ret_to_user
ret_to_user:
    kernel_exit 0
