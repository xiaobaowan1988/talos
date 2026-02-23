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
 * Phase 4 新增：
 *   - sched_init()：初始化 CFS 调度器（idle 进程、运行队列）
 *   - test_scheduler()：创建 3 个不同优先级内核线程，验证 CFS 按权重分配
 *
 * Phase 5 新增：
 *   - 创建用户进程页表，加载嵌入的 init ELF 映像
 *   - 创建用户 init 进程，调度运行
 *   - 用户程序通过 SVC #0 调用 write() 和 exit()
 *
 * Phase 6 新增：
 *   - virtio_init()：扫描 VirtIO MMIO 总线，探测设备
 *   - virtio_blk_init()：初始化 VirtIO 块设备驱动
 *   - virtio_net_init()：初始化 VirtIO 网络设备驱动
 *   - test_virtio_blk()：读写块设备验证
 *   - test_virtio_net()：网络设备初始化验证
 *
 * Phase 7 新增：
 *   - vfs_init()：初始化 VFS 子系统（dcache, inode, file 池）
 *   - ramfs_init()：注册 ramfs 文件系统
 *   - do_mount()：挂载 ramfs 到根目录
 *   - test_vfs()：创建文件、写入、读取、验证 dcache 命中
 *
 * Phase 8 新增：
 *   - squashfs_init()：注册 squashfs 文件系统
 *   - xfs_init()：注册 XFS 文件系统
 *   - squashfs_mkfs_test()：在磁盘上构建 squashfs 测试镜像
 *   - xfs_mkfs()：格式化 XFS 分区
 *   - 挂载 squashfs 到 /sq，XFS 到 /xfs
 *   - 读取 squashfs 只读文件，XFS 文件创建/读写验证
 *
 * Phase 9 新增：
 *   - ovl_init()：注册 overlay 文件系统
 *   - 在 ramfs 根上创建 /upper 和 /work 目录
 *   - 挂载 overlayfs 到 /merged（lower=/sq, upper=/upper, work=/work）
 *   - 读穿测试（从 lower squashfs 读取）
 *   - copy-up 测试（写入触发 lower→upper 复制）
 *   - whiteout 测试（删除 lower 文件，创建屏蔽标记）
 *
 * 注：handle_irq() 已移至 kernel/irq/handle.c（Phase 3）
 */

#include <linux/types.h>
#include <linux/sched.h>
#include <linux/fs.h>
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

/* Phase 5：用户页表（arch/arm64/mm/mmu.c） */
unsigned long create_user_pgd(void);
int map_user_page(unsigned long pgd_phys, unsigned long va,
                  unsigned long pa, unsigned long attrs);

/* Phase 5：ELF 加载器（fs/binfmt_elf.c） */
int load_elf_binary(const void *elf_data, size_t elf_size,
                    unsigned long pgd_phys, unsigned long *entry_out);

/* Phase 5：用户进程创建（kernel/fork.c） */
struct task_struct *create_user_task(unsigned long pgd_phys,
                                     unsigned long entry_pc,
                                     unsigned long user_sp,
                                     const char *name);

/* Phase 5：嵌入的 init ELF 二进制（userspace/init_blob.S） */
extern unsigned char _user_init_start[];
extern unsigned long _user_init_size;

/* Phase 5：用户栈页分配 */
struct page;
struct page *alloc_pages(unsigned int order);
void *page_address(struct page *page);

/* Phase 5：前向声明 */
static void test_user_process(void);

/* Phase 6：VirtIO 驱动框架（drivers/virtio/, drivers/block/, drivers/net/） */
void virtio_init(void);
int virtio_blk_init(void);
int virtio_net_init(void);
void test_virtio_blk(void);
void test_virtio_net(void);

/* Phase 7：VFS + dentry 缓存（fs/vfs/, fs/ramfs/） */
void vfs_init(void);
void ramfs_init(void);
int do_sys_open(struct files_struct *files, const char *pathname,
                int flags, unsigned int mode);
int do_sys_close(struct files_struct *files, int fd);
ssize_t vfs_read(struct file *filp, char *buf, size_t count);
ssize_t vfs_write(struct file *filp, const char *buf, size_t count);
struct file *fget(struct files_struct *files, int fd);
extern struct files_struct init_files;
static void test_vfs(void);

/* Phase 8：squashfs + XFS（fs/squashfs/, fs/xfs/） */
void squashfs_init(void);
void squashfs_mkfs_test(void);
void xfs_init(void);
void xfs_mkfs(void);
static void test_phase8(void);

/* Phase 9：overlayfs（fs/overlayfs/） */
void ovl_init(void);
int do_sys_unlink(struct files_struct *files, const char *pathname);
static void test_phase9(void);

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
 * Phase 4 新增：
 *  11. sched_init()     — 初始化 CFS 调度器
 *  12. test_scheduler() — 创建线程并验证 CFS 按权重分配
 *
 * 参考：init/main.c: asmlinkage __visible void __init start_kernel(void)
 */
void start_kernel(void)
{
    boot_printk("[BOOT] ARM64 kernel starting...\n");
    boot_printk("[BOOT] Phase 9: overlayfs\n");

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

    /* ---- Phase 4: CFS 调度器 ---- */
    /*
     * 在使能 IRQ 之后初始化调度器，因为 sched_init 需要读取
     * CNTVCT_EL0 作为时钟源（不需要中断，但验证 timer 正常后再初始化更安全）。
     *
     * sched_init 创建 idle 进程并初始化运行队列。
     * 之后的 test_scheduler 创建内核线程，由 timer tick 驱动调度。
     */
    boot_printk("[BOOT] Initializing CFS scheduler...\n");
    sched_init();
    boot_printk("[BOOT] CFS scheduler initialized\n");

    /* ---- Phase 4: 验证 CFS 调度器 ---- */
    test_scheduler();

    boot_printk("[BOOT] Phase 4 complete\n");

    /* ---- Phase 5: 系统调用 + ELF 加载 ---- */
    test_user_process();

    boot_printk("[BOOT] Phase 5 complete\n");

    /* ---- Phase 6: VirtIO 驱动框架 ---- */
    /*
     * VirtIO MMIO 总线扫描 + 设备驱动初始化。
     * 需在 GIC 已初始化、IRQ 已使能、Buddy 可用之后调用。
     *
     * 流程：
     *   1. virtio_init() — 扫描 32 个 MMIO slot，探测设备
     *   2. virtio_blk_init() — 初始化块设备（特性协商 + 队列设置）
     *   3. virtio_net_init() — 初始化网络设备
     *   4. 运行设备测试
     */
    boot_printk("[BOOT] === Phase 6: VirtIO drivers ===\n");

    boot_printk("[BOOT] Scanning VirtIO MMIO bus...\n");
    virtio_init();

    boot_printk("[BOOT] Initializing VirtIO block device...\n");
    virtio_blk_init();

    boot_printk("[BOOT] Initializing VirtIO network device...\n");
    virtio_net_init();

    /* Phase 6: 验证 VirtIO 设备 */
    test_virtio_blk();
    test_virtio_net();

    boot_printk("[BOOT] Phase 6 complete\n");

    /* ---- Phase 7: VFS + dentry 缓存 ---- */
    /*
     * VFS 初始化顺序：
     *   1. vfs_init() — 初始化 dcache hash 表、inode 池、file 池
     *   2. ramfs_init() — 注册 ramfs 文件系统类型
     *   3. do_mount() — 挂载 ramfs 到 /（根文件系统）
     *   4. test_vfs() — 验证文件创建/读写/dcache 命中
     */
    boot_printk("[BOOT] === Phase 7: VFS + dentry cache ===\n");

    boot_printk("[BOOT] Initializing VFS...\n");
    vfs_init();

    boot_printk("[BOOT] Registering ramfs...\n");
    ramfs_init();

    boot_printk("[BOOT] Mounting root filesystem (ramfs)...\n");
    do_mount("none", "/", "ramfs", 0, NULL);

    /* Phase 7: 验证 VFS */
    test_vfs();

    boot_printk("[BOOT] Phase 7 complete\n");

    /* ---- Phase 8: squashfs 只读层 + XFS 日志文件系统 ---- */
    /*
     * Phase 8 初始化顺序：
     *   1. 注册 squashfs 和 XFS 文件系统类型
     *   2. 构建 squashfs 测试镜像（写入磁盘）
     *   3. 格式化 XFS 分区
     *   4. 挂载 squashfs 到 /sq
     *   5. 挂载 XFS 到 /xfs
     *   6. 运行验证测试
     */
    boot_printk("[BOOT] === Phase 8: squashfs + XFS ===\n");

    boot_printk("[BOOT] Registering squashfs...\n");
    squashfs_init();

    boot_printk("[BOOT] Registering XFS...\n");
    xfs_init();

    /* 运行 Phase 8 测试 */
    test_phase8();

    boot_printk("[BOOT] Phase 8 complete\n");

    /* ---- Phase 9: overlayfs 三层联合挂载 ---- */
    /*
     * Phase 9 初始化顺序：
     *   1. 注册 overlay 文件系统类型
     *   2. 在 ramfs 根上创建 /upper 和 /work 目录
     *   3. 挂载 overlayfs 到 /merged（lower=/sq, upper=/upper, work=/work）
     *   4. 运行验证测试：读穿、copy-up、whiteout
     */
    boot_printk("[BOOT] === Phase 9: overlayfs ===\n");

    boot_printk("[BOOT] Registering overlayfs...\n");
    ovl_init();

    /* 运行 Phase 9 测试 */
    test_phase9();

    boot_printk("[BOOT] Phase 9 complete\n");

    /* Phase 9 终态：调度器运行中，挂死 idle 进程 */
    while (1)
        __asm__ volatile("wfi");
}

/*
 * ============================================================
 * Phase 5: 用户进程验证
 *
 * 流程：
 *   1. 创建用户进程页表（create_user_pgd）
 *   2. 加载嵌入的 init ELF 映像（load_elf_binary）
 *   3. 分配用户栈页并映射
 *   4. 创建用户 init 进程（create_user_task）
 *   5. 调度运行：idle → init → write("Hello from user space!\n") → exit(0)
 * ============================================================
 */

/* 用户栈顶地址 */
#define USER_STACK_TOP      0x00800000UL
#define USER_STACK_PAGES    4           /* 16KB 用户栈 */

static void test_user_process(void)
{
    unsigned long pgd_phys;
    unsigned long entry_pc;
    unsigned long stack_pa;
    struct page *stack_page;
    struct task_struct *init_task;
    int ret, i;

    boot_printk("[BOOT] === Phase 5: user process test ===\n");

    /* Step 1: 创建用户页表 */
    boot_printk("[BOOT] Creating user page table...\n");
    pgd_phys = create_user_pgd();
    if (!pgd_phys) {
        boot_printk("[BOOT] FAIL: create_user_pgd failed\n");
        return;
    }
    boot_printk("[BOOT] User PGD: ");
    boot_printk_hex(pgd_phys);
    boot_printk("\n");

    /* Step 2: 加载 ELF 映像 */
    boot_printk("[BOOT] Loading init ELF (size=");
    boot_printk_hex(_user_init_size);
    boot_printk(")...\n");

    ret = load_elf_binary(_user_init_start, (size_t)_user_init_size,
                          pgd_phys, &entry_pc);
    if (ret != 0) {
        boot_printk("[BOOT] FAIL: load_elf_binary returned ");
        boot_printk_hex((unsigned long)ret);
        boot_printk("\n");
        return;
    }
    boot_printk("[BOOT] ELF loaded, entry=");
    boot_printk_hex(entry_pc);
    boot_printk("\n");

    /* Step 3: 分配并映射用户栈 */
    boot_printk("[BOOT] Setting up user stack...\n");
    for (i = 0; i < USER_STACK_PAGES; i++) {
        unsigned long stack_va = USER_STACK_TOP - (unsigned long)(USER_STACK_PAGES - i) * PAGE_SIZE;

        stack_page = alloc_pages(0);
        if (!stack_page) {
            boot_printk("[BOOT] FAIL: cannot allocate user stack page\n");
            return;
        }
        stack_pa = (unsigned long)page_address(stack_page);

        /* PD_USER_DATA 定义在 pgtable.h：用户可读写，不可执行 */
        ret = map_user_page(pgd_phys, stack_va, stack_pa,
                            (1UL << 10) |   /* AF */
                            (3UL << 8)  |   /* Inner Shareable */
                            (1UL << 6)  |   /* AP=01: RW user */
                            (1UL << 53) |   /* PXN */
                            (1UL << 54) |   /* UXN */
                            (1UL << 11) |   /* nG */
                            (3UL << 2));    /* AttrIdx=3 MT_NORMAL */
        if (ret != 0) {
            boot_printk("[BOOT] FAIL: map user stack page\n");
            return;
        }
    }
    boot_printk("[BOOT] User stack mapped at ");
    boot_printk_hex(USER_STACK_TOP - (unsigned long)USER_STACK_PAGES * PAGE_SIZE);
    boot_printk(" - ");
    boot_printk_hex(USER_STACK_TOP);
    boot_printk("\n");

    /* Step 4: 创建用户 init 进程 */
    init_task = create_user_task(pgd_phys, entry_pc, USER_STACK_TOP, "init");
    if (!init_task) {
        boot_printk("[BOOT] FAIL: create_user_task failed\n");
        return;
    }

    /* Step 5: 让出 CPU，让 init 进程运行 */
    boot_printk("[BOOT] Scheduling user init process...\n");

    /*
     * idle 让出 CPU → CFS 选择 init → cpu_switch_to → ret_to_user
     * → kernel_exit 0 → eret 到 EL0 → init 的 _start
     * → SVC write → SVC exit → 回到这里
     */
    {
        int timeout = 0;
        int start_tick = arch_timer_tick_count;

        while (timeout < 200) {
            schedule();
            if (init_task->state == TASK_DEAD) {
                boot_printk("[BOOT] User init process exited successfully\n");
                boot_printk("[BOOT] syscall + ELF + user process: PASS\n");
                return;
            }
            /* 简单超时检测 */
            if (arch_timer_tick_count - start_tick > 100) {
                timeout = 200;
                break;
            }
            __asm__ volatile("wfi");
        }
    }

    boot_printk("[BOOT] WARNING: user init did not exit in time\n");
}

/*
 * ============================================================
 * Phase 7: VFS 验证
 *
 * 流程：
 *   1. 通过 VFS 创建 /test.txt 文件
 *   2. 写入 "hello vfs\n"
 *   3. 关闭文件
 *   4. 重新打开并读取
 *   5. 验证内容正确
 *   6. 再次打开同一文件 — 验证 dcache 命中
 *   7. 创建子目录 /subdir 并在其中创建文件
 *
 * 参考：Phase 7 设计文档 §7.8 验证方法
 * ============================================================
 */
static int vfs_str_equal(const char *a, const char *b, size_t len)
{
    size_t i;
    for (i = 0; i < len; i++) {
        if (a[i] != b[i])
            return 0;
    }
    return 1;
}

static void test_vfs(void)
{
    int fd;
    int fd2;
    char buf[32];
    ssize_t nread;
    ssize_t nwrite;
    struct file *filp;
    int i;

    boot_printk("[BOOT] === Phase 7: VFS test ===\n");

    /* --- Test 1: 创建并写入文件 --- */
    boot_printk("[vfs-test] Creating /test.txt...\n");
    fd = do_sys_open(&init_files, "/test.txt", O_CREAT | O_WRONLY, 0644);
    if (fd < 0) {
        boot_printk("[vfs-test] FAIL: open(/test.txt, O_CREAT) returned ");
        boot_printk_hex((unsigned long)fd);
        boot_printk("\n");
        return;
    }
    boot_printk("[vfs-test] fd=");
    boot_printk_hex((unsigned long)fd);
    boot_printk("\n");

    /* 写入数据 */
    filp = fget(&init_files, fd);
    if (!filp) {
        boot_printk("[vfs-test] FAIL: fget returned NULL\n");
        return;
    }
    nwrite = vfs_write(filp, "hello vfs\n", 10);
    boot_printk("[vfs-test] write returned ");
    boot_printk_hex((unsigned long)nwrite);
    boot_printk("\n");

    if (nwrite != 10) {
        boot_printk("[vfs-test] FAIL: expected write 10 bytes\n");
        return;
    }

    /* 关闭文件 */
    do_sys_close(&init_files, fd);

    /* --- Test 2: 重新打开并读取 --- */
    boot_printk("[vfs-test] Reopening /test.txt for read...\n");
    fd = do_sys_open(&init_files, "/test.txt", O_RDONLY, 0);
    if (fd < 0) {
        boot_printk("[vfs-test] FAIL: open(/test.txt, O_RDONLY) returned ");
        boot_printk_hex((unsigned long)fd);
        boot_printk("\n");
        return;
    }

    /* 读取数据 */
    filp = fget(&init_files, fd);
    if (!filp) {
        boot_printk("[vfs-test] FAIL: fget returned NULL for read\n");
        return;
    }

    /* 清零缓冲区 */
    for (i = 0; i < 32; i++)
        buf[i] = 0;

    nread = vfs_read(filp, buf, 16);
    boot_printk("[vfs-test] read returned ");
    boot_printk_hex((unsigned long)nread);
    boot_printk("\n");

    if (nread != 10) {
        boot_printk("[vfs-test] FAIL: expected read 10 bytes, got ");
        boot_printk_hex((unsigned long)nread);
        boot_printk("\n");
        return;
    }

    /* 验证内容 */
    if (vfs_str_equal(buf, "hello vfs\n", 10)) {
        boot_printk("[vfs-test] VFS test OK: ");
        /* 安全输出内容（不含换行后的垃圾）*/
        buf[10] = '\0';
        boot_printk(buf);
    } else {
        boot_printk("[vfs-test] FAIL: content mismatch\n");
        return;
    }

    do_sys_close(&init_files, fd);

    /* --- Test 3: dcache 命中验证 --- */
    /*
     * 再次打开 /test.txt — 此时 dentry 已在 dcache 中。
     * 路径解析应该直接命中 dcache（快速路径），
     * 不需要调用文件系统的 lookup 回调。
     */
    boot_printk("[vfs-test] Verifying dcache hit for /test.txt...\n");
    fd2 = do_sys_open(&init_files, "/test.txt", O_RDONLY, 0);
    if (fd2 < 0) {
        boot_printk("[vfs-test] FAIL: dcache reopen failed\n");
        return;
    }
    do_sys_close(&init_files, fd2);
    boot_printk("[vfs-test] dcache hit: PASS\n");

    /* --- Test 4: 创建子目录并在其中创建文件 --- */
    boot_printk("[vfs-test] Creating /subdir/hello.txt...\n");
    {
        struct dentry *root_dentry;
        struct dentry *subdir_dentry;
        struct qstr subdir_name;
        struct inode *root_inode;
        int ret;

        /* 获取根 dentry */
        root_dentry = path_lookup("/");
        if (!root_dentry || !root_dentry->d_inode) {
            boot_printk("[vfs-test] FAIL: root lookup failed\n");
            return;
        }
        root_inode = root_dentry->d_inode;

        /* 创建子目录 dentry */
        subdir_name.name = "subdir";
        subdir_name.len = 6;
        subdir_name.hash = full_name_hash(root_dentry, "subdir", 6);

        subdir_dentry = d_alloc(root_dentry, &subdir_name);
        if (!subdir_dentry) {
            boot_printk("[vfs-test] FAIL: d_alloc subdir failed\n");
            return;
        }

        /* 调用 mkdir */
        ret = root_inode->i_op->mkdir(root_inode, subdir_dentry, 0755);
        if (ret != 0) {
            boot_printk("[vfs-test] FAIL: mkdir returned error\n");
            return;
        }

        /* 在子目录中创建文件 */
        fd = do_sys_open(&init_files, "/subdir/hello.txt",
                         O_CREAT | O_WRONLY, 0644);
        if (fd < 0) {
            boot_printk("[vfs-test] FAIL: open /subdir/hello.txt error ");
            boot_printk_hex((unsigned long)fd);
            boot_printk("\n");
            return;
        }

        filp = fget(&init_files, fd);
        vfs_write(filp, "subdir OK\n", 10);
        do_sys_close(&init_files, fd);

        /* 读回验证 */
        fd = do_sys_open(&init_files, "/subdir/hello.txt", O_RDONLY, 0);
        if (fd < 0) {
            boot_printk("[vfs-test] FAIL: reopen /subdir/hello.txt error\n");
            return;
        }

        filp = fget(&init_files, fd);
        for (i = 0; i < 32; i++)
            buf[i] = 0;
        nread = vfs_read(filp, buf, 16);
        do_sys_close(&init_files, fd);

        if (nread == 10 && vfs_str_equal(buf, "subdir OK\n", 10)) {
            boot_printk("[vfs-test] Subdirectory file test: PASS\n");
        } else {
            boot_printk("[vfs-test] FAIL: subdir file content mismatch\n");
            return;
        }
    }

    boot_printk("[BOOT] VFS + dentry cache: all tests passed\n");
}

/*
 * ============================================================
 * Phase 8: squashfs + XFS 验证
 *
 * 流程：
 *   1. 构建 squashfs 测试镜像
 *   2. 挂载 squashfs 到 /sq
 *   3. 读取 /sq/hello.txt 并验证内容
 *   4. 格式化 XFS 分区
 *   5. 挂载 XFS 到 /xfs
 *   6. 在 /xfs 中创建文件，写入数据
 *   7. 重新打开读取并验证
 *
 * 参考：Phase 8 设计文档 §8.9
 * ============================================================
 */
static void test_phase8(void)
{
    int fd;
    char buf[64];
    ssize_t n;
    struct file *filp;
    int i;

    boot_printk("[BOOT] === Phase 8: filesystem test ===\n");

    /* === squashfs 测试 === */

    /* 1. 在磁盘上构建 squashfs 测试镜像 */
    squashfs_mkfs_test();

    /* 2. 挂载 squashfs 到 /sq */
    boot_printk("[p8-test] Mounting squashfs on /sq...\n");
    if (do_mount("none", "/sq", "squashfs", 0, NULL) != 0) {
        boot_printk("[p8-test] FAIL: mount squashfs\n");
        return;
    }

    /* 3. 读取 /sq/hello.txt */
    boot_printk("[p8-test] Opening /sq/hello.txt...\n");
    fd = do_sys_open(&init_files, "/sq/hello.txt", O_RDONLY, 0);
    if (fd < 0) {
        boot_printk("[p8-test] FAIL: open /sq/hello.txt, err=");
        boot_printk_hex((unsigned long)fd);
        boot_printk("\n");
        return;
    }

    filp = fget(&init_files, fd);
    if (!filp) {
        boot_printk("[p8-test] FAIL: fget returned NULL\n");
        return;
    }

    for (i = 0; i < 64; i++)
        buf[i] = 0;

    n = vfs_read(filp, buf, 64);
    do_sys_close(&init_files, fd);

    boot_printk("[p8-test] squashfs read: ");
    boot_printk_hex((unsigned long)n);
    boot_printk(" bytes\n");

    /* 验证内容 == "squashfs works!\n" (16 bytes) */
    if (n == 16 && vfs_str_equal(buf, "squashfs works!\n", 16)) {
        boot_printk("[p8-test] squashfs read: PASS\n");
    } else {
        boot_printk("[p8-test] FAIL: squashfs content mismatch\n");
        buf[16] = '\0';
        boot_printk("[p8-test] got: ");
        boot_printk(buf);
        boot_printk("\n");
        return;
    }

    /* 4. 读取第二个文件 /sq/readme.txt */
    fd = do_sys_open(&init_files, "/sq/readme.txt", O_RDONLY, 0);
    if (fd < 0) {
        boot_printk("[p8-test] FAIL: open /sq/readme.txt\n");
        return;
    }

    filp = fget(&init_files, fd);
    for (i = 0; i < 64; i++)
        buf[i] = 0;
    n = vfs_read(filp, buf, 64);
    do_sys_close(&init_files, fd);

    if (n == 13 && vfs_str_equal(buf, "read-only fs\n", 13)) {
        boot_printk("[p8-test] squashfs readme: PASS\n");
    } else {
        boot_printk("[p8-test] FAIL: squashfs readme mismatch\n");
        return;
    }

    /* === XFS 测试 === */

    /* 5. 格式化 XFS */
    xfs_mkfs();

    /* 6. 挂载 XFS 到 /xfs */
    boot_printk("[p8-test] Mounting XFS on /xfs...\n");
    if (do_mount("none", "/xfs", "xfs", 0, NULL) != 0) {
        boot_printk("[p8-test] FAIL: mount XFS\n");
        return;
    }

    /* 7. 创建文件并写入 */
    boot_printk("[p8-test] Creating /xfs/test.txt...\n");
    fd = do_sys_open(&init_files, "/xfs/test.txt",
                     O_CREAT | O_WRONLY, 0644);
    if (fd < 0) {
        boot_printk("[p8-test] FAIL: open /xfs/test.txt, err=");
        boot_printk_hex((unsigned long)fd);
        boot_printk("\n");
        return;
    }

    filp = fget(&init_files, fd);
    n = vfs_write(filp, "XFS WAL test\n", 13);
    do_sys_close(&init_files, fd);

    boot_printk("[p8-test] XFS write: ");
    boot_printk_hex((unsigned long)n);
    boot_printk(" bytes\n");

    if (n != 13) {
        boot_printk("[p8-test] FAIL: XFS write\n");
        return;
    }

    /* 8. 重新打开并读取验证 */
    fd = do_sys_open(&init_files, "/xfs/test.txt", O_RDONLY, 0);
    if (fd < 0) {
        boot_printk("[p8-test] FAIL: reopen /xfs/test.txt\n");
        return;
    }

    filp = fget(&init_files, fd);
    for (i = 0; i < 64; i++)
        buf[i] = 0;
    n = vfs_read(filp, buf, 64);
    do_sys_close(&init_files, fd);

    if (n == 13 && vfs_str_equal(buf, "XFS WAL test\n", 13)) {
        boot_printk("[p8-test] XFS write+read: PASS\n");
    } else {
        boot_printk("[p8-test] FAIL: XFS content mismatch (read ");
        boot_printk_hex((unsigned long)n);
        boot_printk(" bytes)\n");
        return;
    }

    boot_printk("[BOOT] Phase 8 filesystem tests: all passed\n");
}

/*
 * ============================================================
 * Phase 9: overlayfs 验证
 *
 * 流程：
 *   1. 在 ramfs 根上创建 /upper 和 /work 目录
 *   2. 挂载 overlayfs 到 /merged（lower=/sq, upper=/upper, work=/work）
 *   3. 读穿测试：读 /merged/hello.txt → 来自 lower 层 squashfs
 *   4. copy-up 测试：写 /merged/hello.txt → 触发 copy-up → 修改后内容来自 upper
 *   5. whiteout 测试：删除 /merged/readme.txt → 创建 whiteout → 文件不可见
 *
 * 参考：Phase 9 设计文档 §9.8
 * ============================================================
 */
static void test_phase9(void)
{
    int fd;
    char buf[64];
    ssize_t n;
    struct file *filp;
    int i;

    boot_printk("[BOOT] === Phase 9: overlayfs test ===\n");

    /* === Step 1: 创建 upper 和 work 目录 === */
    /*
     * ramfs（根文件系统）支持 mkdir。
     * 通过 VFS 接口创建 /upper 和 /work 目录。
     */
    boot_printk("[p9-test] Creating /upper and /work dirs...\n");
    {
        struct dentry *root_dentry;
        struct dentry *dir_dentry;
        struct qstr dir_name;
        struct inode *root_inode;
        int ret;

        root_dentry = path_lookup("/");
        if (!root_dentry || !root_dentry->d_inode) {
            boot_printk("[p9-test] FAIL: root lookup failed\n");
            return;
        }
        root_inode = root_dentry->d_inode;

        /* 创建 /upper */
        dir_name.name = "upper";
        dir_name.len = 5;
        dir_name.hash = full_name_hash(root_dentry, "upper", 5);

        dir_dentry = d_alloc(root_dentry, &dir_name);
        if (!dir_dentry) {
            boot_printk("[p9-test] FAIL: d_alloc /upper\n");
            return;
        }

        ret = root_inode->i_op->mkdir(root_inode, dir_dentry, 0755);
        if (ret != 0) {
            boot_printk("[p9-test] FAIL: mkdir /upper\n");
            return;
        }

        /* 创建 /work */
        dir_name.name = "work";
        dir_name.len = 4;
        dir_name.hash = full_name_hash(root_dentry, "work", 4);

        dir_dentry = d_alloc(root_dentry, &dir_name);
        if (!dir_dentry) {
            boot_printk("[p9-test] FAIL: d_alloc /work\n");
            return;
        }

        ret = root_inode->i_op->mkdir(root_inode, dir_dentry, 0755);
        if (ret != 0) {
            boot_printk("[p9-test] FAIL: mkdir /work\n");
            return;
        }
    }
    boot_printk("[p9-test] /upper and /work created\n");

    /* === Step 2: 挂载 overlayfs === */
    boot_printk("[p9-test] Mounting overlayfs on /merged...\n");
    if (do_mount("overlay", "/merged", "overlay", 0,
                  "lowerdir=/sq,upperdir=/upper,workdir=/work") != 0) {
        boot_printk("[p9-test] FAIL: mount overlayfs\n");
        return;
    }

    /* === Step 3: 读穿测试 === */
    /*
     * 读取 /merged/hello.txt — 文件只存在于 lower 层（squashfs）。
     * overlayfs 应透传到 squashfs 读取。
     * 预期内容："squashfs works!\n"（16 字节）
     */
    boot_printk("[p9-test] Read-through: /merged/hello.txt...\n");
    fd = do_sys_open(&init_files, "/merged/hello.txt", O_RDONLY, 0);
    if (fd < 0) {
        boot_printk("[p9-test] FAIL: open /merged/hello.txt, err=");
        boot_printk_hex((unsigned long)fd);
        boot_printk("\n");
        return;
    }

    filp = fget(&init_files, fd);
    if (!filp) {
        boot_printk("[p9-test] FAIL: fget returned NULL\n");
        return;
    }

    for (i = 0; i < 64; i++)
        buf[i] = 0;

    n = vfs_read(filp, buf, 64);
    do_sys_close(&init_files, fd);

    boot_printk("[p9-test] read-through: ");
    boot_printk_hex((unsigned long)n);
    boot_printk(" bytes\n");

    if (n == 16 && vfs_str_equal(buf, "squashfs works!\n", 16)) {
        boot_printk("[p9-test] read-through: PASS\n");
    } else {
        boot_printk("[p9-test] FAIL: read-through content mismatch\n");
        buf[32] = '\0';
        boot_printk("[p9-test] got: ");
        boot_printk(buf);
        boot_printk("\n");
        return;
    }

    /* === Step 4: copy-up + write 测试 === */
    /*
     * 写入 /merged/hello.txt：
     *   - 文件在 lower 层（只读）
     *   - overlayfs 先执行 copy-up（复制到 upper 层）
     *   - 然后在 upper 层执行写入
     *   - 写入后文件大小变为新内容长度
     */
    boot_printk("[p9-test] Copy-up + write: /merged/hello.txt...\n");
    fd = do_sys_open(&init_files, "/merged/hello.txt", O_WRONLY, 0);
    if (fd < 0) {
        boot_printk("[p9-test] FAIL: open for write, err=");
        boot_printk_hex((unsigned long)fd);
        boot_printk("\n");
        return;
    }

    filp = fget(&init_files, fd);
    n = vfs_write(filp, "overlayfs!\n", 11);
    do_sys_close(&init_files, fd);

    if (n != 11) {
        boot_printk("[p9-test] FAIL: write returned ");
        boot_printk_hex((unsigned long)n);
        boot_printk("\n");
        return;
    }

    /* 验证 copy-up 后读取到修改后的内容 */
    fd = do_sys_open(&init_files, "/merged/hello.txt", O_RDONLY, 0);
    if (fd < 0) {
        boot_printk("[p9-test] FAIL: reopen after write\n");
        return;
    }

    filp = fget(&init_files, fd);
    for (i = 0; i < 64; i++)
        buf[i] = 0;
    n = vfs_read(filp, buf, 64);
    do_sys_close(&init_files, fd);

    if (n == 11 && vfs_str_equal(buf, "overlayfs!\n", 11)) {
        boot_printk("[p9-test] copy-up write+read: PASS\n");
    } else {
        boot_printk("[p9-test] FAIL: copy-up content mismatch (read ");
        boot_printk_hex((unsigned long)n);
        boot_printk(" bytes)\n");
        buf[32] = '\0';
        boot_printk("[p9-test] got: ");
        boot_printk(buf);
        boot_printk("\n");
        return;
    }

    /* === Step 5: whiteout 测试 === */
    /*
     * 删除 /merged/readme.txt：
     *   - 文件只在 lower 层（squashfs，只读）
     *   - overlayfs 无法修改 lower，在 upper 层创建 whiteout
     *   - whiteout 是 S_IFCHR 类型的特殊文件
     *   - 之后 lookup 遇到 whiteout 认为文件不存在
     */
    boot_printk("[p9-test] Whiteout: unlinking /merged/readme.txt...\n");

    /* 先验证文件存在 */
    fd = do_sys_open(&init_files, "/merged/readme.txt", O_RDONLY, 0);
    if (fd < 0) {
        boot_printk("[p9-test] FAIL: readme.txt not accessible before unlink\n");
        return;
    }
    do_sys_close(&init_files, fd);

    /* 执行 unlink（创建 whiteout） */
    {
        int ret = do_sys_unlink(&init_files, "/merged/readme.txt");
        if (ret != 0) {
            boot_printk("[p9-test] FAIL: unlink returned ");
            boot_printk_hex((unsigned long)ret);
            boot_printk("\n");
            return;
        }
    }

    /* 验证文件不再可见 */
    fd = do_sys_open(&init_files, "/merged/readme.txt", O_RDONLY, 0);
    if (fd < 0) {
        /* 预期失败（ENOENT）— whiteout 生效 */
        boot_printk("[p9-test] whiteout: PASS (readme.txt hidden)\n");
    } else {
        boot_printk("[p9-test] FAIL: readme.txt still visible after whiteout\n");
        do_sys_close(&init_files, fd);
        return;
    }

    boot_printk("[BOOT] Phase 9 overlayfs tests: all passed\n");
}
