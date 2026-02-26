/* SPDX-License-Identifier: GPL-2.0 */
/*
 * arm64os/kernel/main.c
 *
 * 内核主入口及异常处理桩函数
 *
 * 参考：init/main.c, arch/arm64/kernel/setup.c
 *
 * Phase 1: boot + exception vectors
 * Phase 2: MMU + buddy allocator
 */

#include <linux/types.h>

/* 由 printk.c 提供 */
void boot_printk(const char *s);
void boot_printk_hex(unsigned long val);

/* 由 linker script 定义的符号 */
extern char _text[];
extern char _end[];
extern char _bss_start[];
extern char _bss_end[];

/*
 * boot_args - head.S 中保存的启动参数
 * boot_args[0] = FDT 物理地址（x0）
 */
extern unsigned long boot_args[4];

/* Phase 2: MMU and memory management */
extern void create_page_tables(void);
extern void enable_mmu(void);
extern void memblock_init(void);
extern void memblock_dump_stats(void);
extern void buddy_init(void);
extern void test_buddy(void);

/*
 * pt_regs - 异常发生时的寄存器快照（由 entry.S kernel_entry 宏构建）
 *
 * 与 entry.S 中的 PT_* 偏移量严格对应：
 *   x0-x29: 0..232（每8字节一个寄存器）
 *   lr(x30): 240
 *   sp:      248（用户态SP，Phase 1 未保存，值为不定）
 *   pc:      256（ELR_EL1）
 *   pstate:  264（SPSR_EL1）
 */
struct pt_regs {
    unsigned long regs[31];         /* x0-x30（含LR）*/
    unsigned long sp;               /* 用户态SP（EL0异常时有效）*/
    unsigned long pc;               /* 异常返回地址（ELR_EL1）*/
    unsigned long pstate;           /* 保存的处理器状态（SPSR_EL1）*/
};

/*
 * esr_to_str - 将 ESR_EL1.EC（Exception Class）转为可读字符串
 *
 * 参考：ARMv8-A ARM, Section D13.2.36（ESR_EL1）
 */
static const char *esr_to_str(unsigned long esr)
{
    unsigned int ec = (esr >> 26) & 0x3f;

    switch (ec) {
    case 0x00: return "Unknown reason";
    case 0x01: return "WFI/WFE instruction";
    case 0x07: return "SVE/SIMD/FP access";
    case 0x0e: return "Illegal Execution State";
    case 0x15: return "SVC (AArch64 syscall)";
    case 0x18: return "MSR/MRS/System instruction";
    case 0x20: return "Instruction Abort (lower EL)";
    case 0x21: return "Instruction Abort (current EL)";
    case 0x22: return "PC alignment fault";
    case 0x24: return "Data Abort (lower EL)";
    case 0x25: return "Data Abort (current EL)";
    case 0x26: return "SP alignment fault";
    case 0x2c: return "FP exception (AArch64)";
    case 0x2f: return "SError interrupt";
    case 0x30: return "Breakpoint (lower EL)";
    case 0x31: return "Breakpoint (current EL)";
    case 0x32: return "Software Step (lower EL)";
    case 0x33: return "Software Step (current EL)";
    case 0x34: return "Watchpoint (lower EL)";
    case 0x35: return "Watchpoint (current EL)";
    case 0x3c: return "BRK instruction";
    default:   return "Unknown EC";
    }
}

/*
 * handle_sync_exception - 同步异常 C 处理函数
 *
 * 由 entry.S 中的 el1h_sync / el0_sync 调用，传入 pt_regs 指针。
 *
 * Phase 1：打印异常信息后挂死（无页表，无法恢复）。
 * Phase 2+ 将在此处理缺页异常（page fault）等可恢复异常。
 */
void handle_sync_exception(struct pt_regs *regs)
{
    unsigned long esr, far;

    /* 读取异常综合寄存器（Exception Syndrome Register）*/
    __asm__ volatile("mrs %0, esr_el1" : "=r"(esr));
    /* 读取故障地址寄存器（Fault Address Register）*/
    __asm__ volatile("mrs %0, far_el1" : "=r"(far));

    boot_printk("\n[EXCEPTION] Synchronous exception caught!\n");
    boot_printk("  ESR_EL1: ");
    boot_printk_hex(esr);
    boot_printk(" (");
    boot_printk(esr_to_str(esr));
    boot_printk(")\n");
    boot_printk("  FAR_EL1: ");
    boot_printk_hex(far);
    boot_printk("\n");
    boot_printk("  PC     : ");
    boot_printk_hex(regs->pc);
    boot_printk("\n");
    boot_printk("  PSTATE : ");
    boot_printk_hex(regs->pstate);
    boot_printk("\n");

    /* Phase 1: 挂死（后续 Phase 将实现恢复逻辑）*/
    boot_printk("[PANIC] Unrecoverable in Phase 1 — halting.\n");
    while (1)
        ;
}

/*
 * handle_irq - IRQ 中断 C 处理函数
 *
 * Phase 1：GIC 未初始化，不应有真实 IRQ，打印提示后返回。
 * Phase 3 GIC v3 初始化后将在此驱动中断控制器读取 IAR 并分发。
 */
void handle_irq(struct pt_regs *regs)
{
    (void)regs;
    boot_printk("[IRQ] Spurious interrupt (GIC not initialized in Phase 1)\n");
    /* 无法 EOI，但 kernel_exit 会 eret 返回 */
}

/*
 * panic_unhandled - 不可恢复异常（FIQ, SError, EL1t 异常等）
 *
 * 由 entry.S 中 el1t_*, el1h_fiq, el1h_error 等 handler 调用。
 * 不接收参数（kernel_entry 后立即调用），打印后挂死。
 */
void panic_unhandled(void)
{
    unsigned long esr, far;

    __asm__ volatile("mrs %0, esr_el1" : "=r"(esr));
    __asm__ volatile("mrs %0, far_el1" : "=r"(far));

    boot_printk("\n[PANIC] Unhandled exception!\n");
    boot_printk("  ESR_EL1: ");
    boot_printk_hex(esr);
    boot_printk("\n");
    boot_printk("  FAR_EL1: ");
    boot_printk_hex(far);
    boot_printk("\n");

    while (1)
        ;
}

/*
 * start_kernel - 内核 C 入口点
 *
 * 由 head.S setup_el1 在完成汇编初始化后调用：
 *   1. 打印启动横幅
 *   2. 验证关键地址（内核文本段、BSS、FDT）
 *   3. Phase 1 结束，进入死循环（等待 Phase 2 实现调度器）
 *
 * 参考：init/main.c: asmlinkage __visible void __init start_kernel(void)
 */
void start_kernel(void)
{
    boot_printk("[BOOT] ARM64 kernel starting...\n");
    boot_printk("[BOOT] Phase 1: Boot + Exception Vectors\n");

    /* 打印内核镜像布局 */
    boot_printk("[BOOT] Kernel text   : ");
    boot_printk_hex((unsigned long)_text);
    boot_printk("\n");
    boot_printk("[BOOT] Kernel end    : ");
    boot_printk_hex((unsigned long)_end);
    boot_printk("\n");

    /* 打印 BSS 段地址（已被 head.S 清零）*/
    boot_printk("[BOOT] BSS           : ");
    boot_printk_hex((unsigned long)_bss_start);
    boot_printk(" - ");
    boot_printk_hex((unsigned long)_bss_end);
    boot_printk("\n");

    /* 打印 FDT 地址（由 QEMU 传入，保存在 boot_args[0]）*/
    boot_printk("[BOOT] FDT addr      : ");
    boot_printk_hex(boot_args[0]);
    boot_printk("\n");

    /* 读取并打印 VBAR_EL1（异常向量表基址）— 验证安装成功 */
    {
        unsigned long vbar;
        __asm__ volatile("mrs %0, vbar_el1" : "=r"(vbar));
        boot_printk("[BOOT] VBAR_EL1      : ");
        boot_printk_hex(vbar);
        boot_printk("\n");
    }

    /* 读取当前 EL（验证运行于 EL1）*/
    {
        unsigned long current_el;
        __asm__ volatile("mrs %0, CurrentEL" : "=r"(current_el));
        current_el = (current_el >> 2) & 0x3;
        boot_printk("[BOOT] CurrentEL     : EL");
        /* 输出 EL 数值（Phase 1 只支持个位数）*/
        {
            char el_str[2] = {'0' + (char)current_el, '\0'};
            boot_printk(el_str);
        }
        boot_printk(" (expected: EL1)\n");
    }

    boot_printk("[BOOT] Exception vectors installed\n");
    boot_printk("[BOOT] Phase 1 complete\n");

    /* ---- Phase 2: MMU + Memory Management ---- */
    boot_printk("\n[BOOT] Phase 2: MMU + Buddy Allocator\n");

    /* Step 1: Initialize early memory allocator */
    memblock_init();

    /* Step 2: Build identity-mapped page tables */
    create_page_tables();

    /* Step 3: Enable MMU with identity mapping */
    enable_mmu();

    /* Step 4: Initialize buddy allocator */
    buddy_init();
    memblock_dump_stats();

    /* Step 5: Test buddy allocator */
    test_buddy();

    boot_printk("\n[BOOT] Phase 2 complete — halting\n");

    while (1)
        ;
}
