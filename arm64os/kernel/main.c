/* SPDX-License-Identifier: GPL-2.0 */
/*
 * arm64os/kernel/main.c
 *
 * 内核主入口及异常处理桩函数
 *
 * 参考：init/main.c, arch/arm64/kernel/setup.c
 *
 * Phase 1 实现：
 *   - start_kernel()：打印启动信息，验证异常向量表已安装
 *   - handle_sync_exception()：同步异常处理桩（打印寄存器信息）
 *   - panic_unhandled()：不可恢复异常处理
 *
 * Phase 2 新增：
 *   - mmu_init()：建立恒等映射页表，开启 MMU
 *   - memblock_init()：初始化早期物理内存分配器
 *   - buddy_init()：初始化 Buddy 物理页分配器
 *   - test_buddy()：验证 Buddy 分配/释放/合并正确性
 *
 * Phase 3 新增：
 *   - gicv3_init()：初始化 GIC v3 中断控制器
 *   - arch_timer_init()：初始化 ARM Virtual Timer，注册 PPI #27
 *   - 使能 IRQ（daifclr #2），等待 10 个 tick 验证中断正常工作
 *
 * 注：handle_irq() 已移至 kernel/irq/handle.c（Phase 3）
 */

#include <linux/types.h>
#include <asm/memory.h>

/* 由 printk.c 提供 */
void boot_printk(const char *s);
void boot_printk_hex(unsigned long val);

/* Phase 2：MMU 初始化（arch/arm64/mm/mmu.c + proc.S） */
void mmu_init(void);

/* Phase 2：memblock（mm/memblock.c） */
void memblock_init(phys_addr_t phys_start, phys_addr_t phys_size);

/* Phase 2：Buddy 分配器（mm/page_alloc.c） */
void buddy_init(void);
void test_buddy(void);

/* Phase 3：GIC v3（drivers/irqchip/gic-v3.c） */
void gicv3_init(void);

/* Phase 3：ARM arch timer（drivers/timer/arm_arch_timer.c） */
void arch_timer_init(void);
extern volatile int arch_timer_tick_count;

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

    boot_printk("[PANIC] Unrecoverable — halting.\n");
    while (1)
        ;
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
 *
 * Phase 1:
 *   1. 打印启动横幅
 *   2. 验证关键地址（内核文本段、BSS、FDT）
 *
 * Phase 2 新增：
 *   3. mmu_init()       — 建立恒等映射页表，开启 MMU
 *   4. memblock_init()  — 初始化早期物理内存分配器
 *   5. buddy_init()     — 初始化 Buddy 物理页分配器
 *   6. test_buddy()     — 验证 Buddy 功能
 *
 * Phase 3 新增：
 *   7. gicv3_init()     — 初始化 GIC v3 中断控制器
 *   8. arch_timer_init()— 注册 PPI #27 处理函数，使能并启动计时器
 *   9. daifclr #2       — 开放 IRQ（清除 DAIF I 位）
 *  10. 轮询 tick_count  — 等待 10 个 timer tick 验证中断链路
 *
 * 参考：init/main.c: asmlinkage __visible void __init start_kernel(void)
 */
void start_kernel(void)
{
    boot_printk("[BOOT] ARM64 kernel starting...\n");
    boot_printk("[BOOT] Phase 3: GIC v3 + arch timer\n");

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

    /* ---- Phase 2: MMU 初始化 ---- */
    boot_printk("[BOOT] Initializing MMU (identity mapping)...\n");
    mmu_init();
    boot_printk("[BOOT] MMU enabled (SCTLR_EL1.M = 1)\n");

    /* 验证 SCTLR_EL1.M 已置位 */
    {
        unsigned long sctlr;
        __asm__ volatile("mrs %0, sctlr_el1" : "=r"(sctlr));
        boot_printk("[BOOT] SCTLR_EL1     : ");
        boot_printk_hex(sctlr);
        boot_printk(" (M=");
        boot_printk((sctlr & 1) ? "1" : "0");
        boot_printk(")\n");
    }

    /* ---- Phase 2: 物理内存初始化 ---- */
    boot_printk("[BOOT] Initializing memblock...\n");
    memblock_init(PHYS_OFFSET, PHYS_SIZE);

    boot_printk("[BOOT] Initializing buddy allocator...\n");
    buddy_init();

    /* ---- Phase 2: 验证 Buddy 分配器 ---- */
    test_buddy();

    /* ---- Phase 3: GIC v3 初始化 ---- */
    boot_printk("[BOOT] Initializing GIC v3...\n");
    gicv3_init();
    boot_printk("[BOOT] GIC v3 initialized\n");

    /* ---- Phase 3: arch timer 初始化 ---- */
    boot_printk("[BOOT] Initializing arch timer (Virtual Timer PPI #27)...\n");
    arch_timer_init();
    boot_printk("[BOOT] arch timer started (10ms interval)\n");

    /* ---- Phase 3: 使能 IRQ ---- */
    /*
     * 清除 DAIF.I 位（IRQ 屏蔽），使能中断。
     * 此后 GIC 会将 PPI #27 中断路由到 el1h_irq → handle_irq()。
     *
     * 注意：DAIF.D（Debug）、.A（SError）、.F（FIQ）仍屏蔽，
     *       只开放 IRQ（#2 = I 位）。
     *
     * 参考：arch/arm64/include/asm/irqflags.h: arch_local_irq_enable()
     */
    boot_printk("[BOOT] Enabling IRQ (daifclr #2)...\n");
    __asm__ volatile("msr daifclr, #2" ::: "memory");

    /* ---- Phase 3: 验证 timer tick ---- */
    /*
     * 等待 10 个 timer tick（约 100ms）。
     * arch_timer_tick_count 由 arch_timer_handler() 在中断上下文递增。
     * volatile 确保每次循环都从内存读取最新值（不被编译器优化掉）。
     */
    boot_printk("[BOOT] Waiting for 10 timer ticks...\n");
    while (arch_timer_tick_count < 10)
        ;

    boot_printk("[BOOT] Timer ticks: OK (received >= 10)\n");
    boot_printk("[BOOT] Phase 3 complete\n");
    boot_printk("[BOOT] Phase 4 will add CFS scheduler\n");

    /* Phase 3 终态：计时器持续运行，挂死等待 Phase 4 */
    while (1)
        ;
}
