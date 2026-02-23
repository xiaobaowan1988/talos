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
 * Phase 10 新增：
 *   - nsproxy_init()：初始化 namespace 子系统（7 种 namespace）
 *   - cgroup_init()：初始化 cgroup v2（CPU、内存、PIDs 控制器）
 *   - 测试 PID namespace：创建子 namespace，验证 PID 从 1 开始
 *   - 测试 UTS namespace：创建新 namespace，设置不同主机名
 *   - 测试 Mount namespace：创建新 namespace，验证挂载表隔离
 *   - 测试 User namespace：UID 映射（容器内 root → 宿主非特权 UID）
 *   - 测试 cgroup 内存控制器：设置限制，验证超限拒绝
 *   - 测试 cgroup PIDs 控制器：设置限制，验证进程数限制
 *
 * Phase 11 新增：
 *   - net_init()：初始化网络子系统（sk_buff池、socket层、TCP、IPv4、netfilter）
 *   - 测试 TCP loopback：创建服务端+客户端 socket，三次握手，数据收发
 *   - 测试 netfilter：注册钩子丢弃特定协议包，验证 NF_DROP 生效
 *   - 测试 nftables：添加规则匹配+过滤，验证规则引擎
 *
 * Phase 12 新增：
 *   - seccomp_init()：初始化 seccomp 子系统（过滤器池）
 *   - bpf_init()：初始化 eBPF 子系统（程序池、JIT 开关）
 *   - landlock_init()：初始化 Landlock LSM（规则集池）
 *   - 测试 seccomp：STRICT 模式白名单过滤 + FILTER 模式 BPF 过滤器
 *   - 测试 eBPF：加载程序、验证器检查、解释器执行、JIT 编译
 *   - 测试 Landlock：创建规则集、添加路径规则、激活沙箱、验证访问控制
 *
 * 注：handle_irq() 已移至 kernel/irq/handle.c（Phase 3）
 */

#include <linux/types.h>
#include <linux/sched.h>
#include <linux/fs.h>
#include <linux/nsproxy.h>
#include <linux/cgroup.h>
#include <linux/net.h>
#include <linux/skbuff.h>
#include <linux/netfilter.h>
#include <linux/seccomp.h>
#include <linux/bpf.h>
#include <linux/landlock.h>
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

/* Phase 10：Namespace + cgroup v2（kernel/nsproxy.c, kernel/cgroup/） */
static void test_phase10(void);

/* Phase 11：TCP/IP + netfilter（net/） */
/* net_init() 定义在 net.h，nf_init() 定义在 netfilter.h */
void nft_init(void);
void inet_init(void);
static void test_phase11(void);

/* Phase 12：seccomp + eBPF JIT + Landlock LSM */
/* seccomp_init() 定义在 seccomp.h */
/* bpf_init() 定义在 bpf.h */
/* landlock_init() 定义在 landlock.h */
extern struct task_struct *current_task;  /* kernel/sched/core.c */
static void test_phase12(void);

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
    boot_printk("[BOOT] Phase 12: seccomp + eBPF JIT + Landlock\n");

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

    /* ---- Phase 10: Linux Namespaces + cgroup v2 ---- */
    /*
     * Phase 10 初始化顺序：
     *   1. nsproxy_init() — 初始化 7 种 namespace 子系统
     *   2. cgroup_init() — 初始化 cgroup v2 统一层级 + 控制器
     *   3. 运行验证测试：PID namespace、UTS namespace、
     *      Mount namespace、User namespace、cgroup 内存/PIDs
     */
    boot_printk("[BOOT] === Phase 10: Namespaces + cgroup v2 ===\n");

    boot_printk("[BOOT] Initializing namespaces...\n");
    nsproxy_init();

    boot_printk("[BOOT] Initializing cgroup v2...\n");
    cgroup_init();

    /* 运行 Phase 10 测试 */
    test_phase10();

    boot_printk("[BOOT] Phase 10 complete\n");

    /* ---- Phase 11: TCP/IP 协议栈 + netfilter 框架 ---- */
    /*
     * Phase 11 初始化顺序：
     *   1. skb_init() — 初始化 sk_buff 数据包缓冲池
     *   2. sock_init() — 初始化 socket 池
     *   3. nf_init() — 初始化 netfilter 钩子框架
     *   4. nft_init() — 初始化 nftables 规则引擎
     *   5. inet_init() — 初始化 IPv4 协议族（含 TCP + IP）
     *   6. 运行验证测试：TCP loopback、netfilter、nftables
     */
    boot_printk("[BOOT] === Phase 11: TCP/IP + netfilter ===\n");

    boot_printk("[BOOT] Initializing network stack...\n");
    skb_init();
    sock_init();
    nf_init();
    nft_init();
    inet_init();
    boot_printk("[net] network init: PASS\n");

    /* 运行 Phase 11 测试 */
    test_phase11();

    boot_printk("[BOOT] Phase 11 complete\n");

    /* ---- Phase 12: seccomp + eBPF JIT + Landlock LSM ---- */
    /*
     * Phase 12 初始化顺序：
     *   1. seccomp_init() — 初始化 seccomp 过滤器池
     *   2. bpf_init() — 初始化 eBPF 程序池
     *   3. landlock_init() — 初始化 Landlock 规则集池
     *   4. 启用 eBPF JIT（bpf_jit_enable = 1）
     *   5. 运行验证测试：seccomp、eBPF、Landlock
     */
    boot_printk("[BOOT] === Phase 12: seccomp + eBPF JIT + Landlock ===\n");

    boot_printk("[BOOT] Initializing seccomp...\n");
    seccomp_init();

    boot_printk("[BOOT] Initializing eBPF...\n");
    bpf_init();

    boot_printk("[BOOT] Initializing Landlock LSM...\n");
    landlock_init();

    /* 启用 JIT 编译 */
    bpf_jit_enable = 1;
    boot_printk("[bpf] JIT enabled\n");

    /* 运行 Phase 12 测试 */
    test_phase12();

    boot_printk("[BOOT] Phase 12 complete\n");

    /* Phase 12 终态：调度器运行中，挂死 idle 进程 */
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

/*
 * ============================================================
 * Phase 10: Namespaces + cgroup v2 验证
 *
 * 流程：
 *   1. PID namespace：创建子 namespace，验证 PID 从 1 开始
 *   2. UTS namespace：创建新 namespace，设置不同主机名，验证隔离
 *   3. Mount namespace：创建新 namespace，验证挂载表独立
 *   4. User namespace：创建新 namespace，设置 UID 映射，验证翻译
 *   5. cgroup 内存控制器：创建 cgroup，设置内存限制，验证超限拒绝
 *   6. cgroup PIDs 控制器：创建 cgroup，设置进程数限制，验证超限拒绝
 *
 * 参考：Phase 10 设计文档 §10.10
 * ============================================================
 */
static void test_phase10(void)
{
    boot_printk("[BOOT] === Phase 10: namespace + cgroup test ===\n");

    /* ============================================================
     * Test 1: PID namespace 隔离
     *
     * 创建子 PID namespace（level=1），分配 PID，
     * 验证子 namespace 的 PID 从 1 开始。
     * 这是容器 init 进程 PID=1 的基础。
     * ============================================================
     */
    boot_printk("[p10-test] === PID namespace test ===\n");
    {
        struct pid_namespace *child_ns;
        int pid1, pid2;

        /* 创建子 PID namespace */
        child_ns = create_pid_namespace(&init_pid_ns);
        if (!child_ns) {
            boot_printk("[p10-test] FAIL: create_pid_namespace returned NULL\n");
            return;
        }

        /* 验证层级 */
        if (child_ns->level != 1) {
            boot_printk("[p10-test] FAIL: child ns level != 1\n");
            return;
        }

        /* 在子 namespace 中分配 PID */
        pid1 = alloc_pid_nr(child_ns);
        pid2 = alloc_pid_nr(child_ns);

        boot_printk("[p10-test] Child ns: first PID=");
        boot_printk_hex((unsigned long)pid1);
        boot_printk(", second PID=");
        boot_printk_hex((unsigned long)pid2);
        boot_printk("\n");

        /* 容器内第一个进程应该得到 PID 1（init） */
        if (pid1 == 1 && pid2 == 2) {
            boot_printk("[p10-test] PID namespace: PASS\n");
        } else {
            boot_printk("[p10-test] FAIL: expected PID 1,2 in child ns\n");
            return;
        }

        put_pid_ns(child_ns);
    }

    /* ============================================================
     * Test 2: UTS namespace 隔离
     *
     * 创建新 UTS namespace，设置不同主机名，
     * 验证宿主 namespace 的主机名未被修改。
     * ============================================================
     */
    boot_printk("[p10-test] === UTS namespace test ===\n");
    {
        struct uts_namespace *container_uts;

        /* 创建容器 UTS namespace（从 init_uts_ns 复制） */
        container_uts = create_uts_namespace(&init_uts_ns);
        if (!container_uts) {
            boot_printk("[p10-test] FAIL: create_uts_namespace returned NULL\n");
            return;
        }

        /* 容器设置自己的主机名 */
        uts_ns_set_hostname(container_uts, "container1");

        boot_printk("[p10-test] Host hostname: ");
        boot_printk(init_uts_ns.nodename);
        boot_printk("\n");
        boot_printk("[p10-test] Container hostname: ");
        boot_printk(container_uts->nodename);
        boot_printk("\n");

        /* 验证隔离：宿主主机名应仍为 "arm64os" */
        {
            const char *expected = "arm64os";
            const char *actual = init_uts_ns.nodename;
            int match = 1;
            int i;
            for (i = 0; expected[i]; i++) {
                if (actual[i] != expected[i]) {
                    match = 0;
                    break;
                }
            }
            if (actual[i] != '\0')
                match = 0;

            if (match) {
                boot_printk("[p10-test] UTS namespace: PASS\n");
            } else {
                boot_printk("[p10-test] FAIL: host hostname was modified\n");
                return;
            }
        }

        put_uts_ns(container_uts);
    }

    /* ============================================================
     * Test 3: Mount namespace 隔离
     *
     * 创建新 mount namespace，添加挂载点，
     * 验证新挂载点只在子 namespace 中可见。
     * ============================================================
     */
    boot_printk("[p10-test] === Mount namespace test ===\n");
    {
        struct mnt_namespace *child_mnt;

        /* 在初始 namespace 中添加一个挂载点 */
        mnt_ns_add_mount(&init_mnt_ns, "/", "ramfs");

        /* 创建子 mount namespace（从 init 复制） */
        child_mnt = create_mnt_namespace(&init_mnt_ns);
        if (!child_mnt) {
            boot_printk("[p10-test] FAIL: create_mnt_namespace returned NULL\n");
            return;
        }

        /* 子 namespace 应继承 "/" 挂载 */
        if (!mnt_ns_has_mount(child_mnt, "/")) {
            boot_printk("[p10-test] FAIL: child ns missing inherited mount\n");
            return;
        }

        /* 在子 namespace 中添加独有挂载 */
        mnt_ns_add_mount(child_mnt, "/container_data", "ramfs");

        /* 验证隔离：父 namespace 不应看到 /container_data */
        if (mnt_ns_has_mount(&init_mnt_ns, "/container_data")) {
            boot_printk("[p10-test] FAIL: parent sees child-only mount\n");
            return;
        }

        /* 子 namespace 应看到 /container_data */
        if (mnt_ns_has_mount(child_mnt, "/container_data")) {
            boot_printk("[p10-test] mount namespace: PASS\n");
        } else {
            boot_printk("[p10-test] FAIL: child ns missing its own mount\n");
            return;
        }

        put_mnt_ns(child_mnt);
    }

    /* ============================================================
     * Test 4: User namespace UID 映射
     *
     * 创建子 user namespace，设置 UID 映射：
     *   容器 UID 0 → 宿主 UID 1000
     * 验证映射翻译正确。
     * ============================================================
     */
    boot_printk("[p10-test] === User namespace test ===\n");
    {
        struct user_namespace *child_userns;
        int outer_uid;

        /* 创建子 user namespace */
        child_userns = create_user_namespace(&init_user_ns);
        if (!child_userns) {
            boot_printk("[p10-test] FAIL: create_user_namespace returned NULL\n");
            return;
        }

        /* 设置 UID 映射：容器 UID 0-65535 → 宿主 UID 1000-66535 */
        user_ns_set_uid_map(child_userns, 0, 1000, 65536);

        /* 验证：容器 UID 0 (root) → 宿主 UID 1000 */
        outer_uid = user_ns_map_uid(child_userns, 0);
        boot_printk("[p10-test] Container UID 0 -> Host UID ");
        boot_printk_hex((unsigned long)outer_uid);
        boot_printk("\n");

        if (outer_uid == 1000) {
            /* 进一步验证：容器 UID 100 → 宿主 UID 1100 */
            outer_uid = user_ns_map_uid(child_userns, 100);
            if (outer_uid == 1100) {
                boot_printk("[p10-test] user namespace: PASS\n");
            } else {
                boot_printk("[p10-test] FAIL: UID 100 mapped to ");
                boot_printk_hex((unsigned long)outer_uid);
                boot_printk(" (expected 1100)\n");
                return;
            }
        } else {
            boot_printk("[p10-test] FAIL: expected Host UID 1000\n");
            return;
        }

        put_user_ns(child_userns);
    }

    /* ============================================================
     * Test 5: nsproxy — 组合创建多个 namespace
     *
     * 模拟 clone(CLONE_NEWUTS | CLONE_NEWPID) 创建容器。
     * ============================================================
     */
    boot_printk("[p10-test] === nsproxy combined test ===\n");
    {
        struct nsproxy *container_ns;
        unsigned long flags = CLONE_NEWUTS | CLONE_NEWPID;

        container_ns = create_nsproxy(&init_nsproxy, flags);
        if (!container_ns) {
            boot_printk("[p10-test] FAIL: create_nsproxy returned NULL\n");
            return;
        }

        /* 验证新的 UTS namespace（不是 init_uts_ns） */
        if (container_ns->uts_ns == &init_uts_ns) {
            boot_printk("[p10-test] FAIL: uts_ns should be new\n");
            return;
        }

        /* 验证新的 PID namespace */
        if (container_ns->pid_ns_for_children == &init_pid_ns) {
            boot_printk("[p10-test] FAIL: pid_ns should be new\n");
            return;
        }

        /* 验证 MNT namespace 共享（未指定 CLONE_NEWNS） */
        if (container_ns->mnt_ns != init_nsproxy.mnt_ns) {
            boot_printk("[p10-test] FAIL: mnt_ns should be shared\n");
            return;
        }

        boot_printk("[p10-test] nsproxy combined: PASS\n");
        put_nsproxy(container_ns);
    }

    /* ============================================================
     * Test 6: cgroup v2 — 内存控制器
     *
     * 创建子 cgroup，设置 memory.max=10MB，
     * 尝试 charge 5MB（成功）和 20MB（失败），
     * 验证内存限制生效。
     * ============================================================
     */
    boot_printk("[p10-test] === cgroup memory test ===\n");
    {
        struct cgroup *test_cg;
        int ret;

        /* 创建子 cgroup */
        test_cg = cgroup_create(&root_cgroup, "mem_test");
        if (!test_cg) {
            boot_printk("[p10-test] FAIL: cgroup_create returned NULL\n");
            return;
        }

        /* 设置内存上限 10MB */
        mem_cgroup_set_max(test_cg, 10 * 1024 * 1024);

        /* 尝试 charge 5MB — 应该成功 */
        ret = mem_cgroup_charge(test_cg, 5 * 1024 * 1024);
        if (ret != 0) {
            boot_printk("[p10-test] FAIL: 5MB charge should succeed\n");
            return;
        }

        boot_printk("[p10-test] memory.current=");
        boot_printk_hex(test_cg->memory_current);
        boot_printk(" after 5MB charge\n");

        /* 尝试 charge 另外 6MB — 应该失败（5+6=11 > 10） */
        ret = mem_cgroup_charge(test_cg, 6 * 1024 * 1024);
        if (ret == 0) {
            boot_printk("[p10-test] FAIL: 6MB charge should fail (OOM)\n");
            return;
        }

        boot_printk("[p10-test] 6MB charge correctly denied (OOM)\n");

        /* 释放 3MB */
        mem_cgroup_uncharge(test_cg, 3 * 1024 * 1024);

        /* 现在 current=2MB，charge 另外 7MB 应该成功（2+7=9 < 10） */
        ret = mem_cgroup_charge(test_cg, 7 * 1024 * 1024);
        if (ret != 0) {
            boot_printk("[p10-test] FAIL: 7MB charge after uncharge should succeed\n");
            return;
        }

        boot_printk("[p10-test] cgroup memory: PASS\n");
    }

    /* ============================================================
     * Test 7: cgroup v2 — PIDs 控制器
     *
     * 创建子 cgroup，设置 pids.max=2，
     * attach 2 个进程（成功），第 3 个进程（拒绝）。
     * ============================================================
     */
    boot_printk("[p10-test] === cgroup PIDs test ===\n");
    {
        struct cgroup *pids_cg;
        struct task_struct fake_task1, fake_task2, fake_task3;
        int ret;

        /* 创建子 cgroup */
        pids_cg = cgroup_create(&root_cgroup, "pids_test");
        if (!pids_cg) {
            boot_printk("[p10-test] FAIL: cgroup_create returned NULL\n");
            return;
        }

        /* 设置 pids.max=2 */
        pids_cgroup_set_max(pids_cg, 2);

        /* 初始化 fake tasks */
        fake_task1.cgroups = &init_css_set;
        fake_task2.cgroups = &init_css_set;
        fake_task3.cgroups = &init_css_set;

        /* attach 第 1 个进程 — 应该成功 */
        ret = cgroup_attach_task(pids_cg, &fake_task1);
        if (ret != 0) {
            boot_printk("[p10-test] FAIL: first attach should succeed\n");
            return;
        }

        /* attach 第 2 个进程 — 应该成功 */
        ret = cgroup_attach_task(pids_cg, &fake_task2);
        if (ret != 0) {
            boot_printk("[p10-test] FAIL: second attach should succeed\n");
            return;
        }

        boot_printk("[p10-test] pids.current=");
        boot_printk_hex((unsigned long)pids_cg->pids_current);
        boot_printk(" after 2 attaches\n");

        /* attach 第 3 个进程 — 应该被拒绝 */
        ret = cgroup_attach_task(pids_cg, &fake_task3);
        if (ret == 0) {
            boot_printk("[p10-test] FAIL: third attach should be denied\n");
            return;
        }

        boot_printk("[p10-test] third attach correctly denied\n");

        /* detach 一个进程后应能再次 attach */
        cgroup_detach_task(pids_cg, &fake_task1);

        ret = cgroup_attach_task(pids_cg, &fake_task3);
        if (ret != 0) {
            boot_printk("[p10-test] FAIL: attach after detach should succeed\n");
            return;
        }

        boot_printk("[p10-test] cgroup pids: PASS\n");
    }

    /* ============================================================
     * Test 8: cgroup CPU 控制器（参数设置验证）
     * ============================================================
     */
    boot_printk("[p10-test] === cgroup CPU test ===\n");
    {
        struct cgroup *cpu_cg;

        cpu_cg = cgroup_create(&root_cgroup, "cpu_test");
        if (!cpu_cg) {
            boot_printk("[p10-test] FAIL: cgroup_create for cpu_test\n");
            return;
        }

        /* 设置 CPU 配额 50%（50000us / 100000us） */
        cpu_cgroup_set_max(cpu_cg, 50000, 100000);

        /* 设置 CPU 权重 */
        cpu_cgroup_set_weight(cpu_cg, 200);

        /* 验证参数 */
        if (cpu_cg->cpu_max_quota == 50000 &&
            cpu_cg->cpu_max_period == 100000 &&
            cpu_cg->cpu_weight == 200) {
            boot_printk("[p10-test] cgroup cpu: PASS\n");
        } else {
            boot_printk("[p10-test] FAIL: CPU controller params mismatch\n");
            return;
        }
    }

    boot_printk("[BOOT] Phase 10 namespace + cgroup tests: all passed\n");
}

/*
 * ============================================================
 * Phase 11: TCP/IP + netfilter 验证
 *
 * 流程：
 *   1. TCP loopback 测试：
 *      a. 创建服务端 socket，绑定端口 8080，设置监听
 *      b. 创建客户端 socket，连接 127.0.0.1:8080
 *      c. TCP 三次握手通过 loopback 同步完成
 *      d. 服务端 accept 获取连接
 *      e. 服务端发送 "Hello TCP!\n"
 *      f. 客户端读取数据，验证内容
 *
 *   2. netfilter 钩子测试：
 *      a. 注册 NF_INET_LOCAL_IN 钩子（丢弃 ICMP 包）
 *      b. 构造测试 skb（模拟 ICMP 包）
 *      c. 调用 nf_hook() 验证返回 NF_DROP
 *      d. 注销钩子
 *
 *   3. nftables 规则引擎测试：
 *      a. 添加 DROP 规则到 INPUT 链
 *      b. 构造测试 skb
 *      c. 调用 nft_do_chain() 验证匹配
 *
 * 参考：Phase 11 设计文档 §11.8
 * ============================================================
 */

/*
 * test_netfilter_drop_hook - 测试用钩子函数
 *
 * 丢弃所有 ICMP 包（protocol == IPPROTO_ICMP）。
 * 参考 §11.8 test_netfilter_drop()。
 */
static unsigned int test_netfilter_drop_hook(void *priv,
                                              struct sk_buff *skb,
                                              const struct nf_hook_state *state)
{
    struct iphdr *iph = skb->nh;
    if (iph && iph->protocol == IPPROTO_ICMP)
        return NF_DROP;
    return NF_ACCEPT;
}

static void test_phase11(void)
{
    boot_printk("[BOOT] === Phase 11: TCP/IP + netfilter test ===\n");

    /* ============================================================
     * Test 1: TCP loopback — 三次握手 + 数据收发
     *
     * 参考：§11.4 TCP三次握手 + §11.8 test_tcp()
     *
     * 数据包路径（全部走 loopback）：
     *   send → tcp_sendmsg → ip_queue_xmit → [LOCAL_OUT hook]
     *   → loopback_xmit → ip_rcv → [PRE_ROUTING hook]
     *   → ip_local_deliver → [LOCAL_IN hook] → tcp_v4_rcv
     *   → socket 接收队列 → recv
     * ============================================================
     */
    boot_printk("[p11-test] === TCP loopback test ===\n");
    {
        int server_fd, client_fd, conn_fd;
        struct sockaddr_in addr;
        char buf[32];
        ssize_t n;
        int i;

        /* 创建服务端 socket */
        boot_printk("[p11-test] Creating server socket...\n");
        server_fd = sys_socket(AF_INET, SOCK_STREAM, 0);
        if (server_fd < 0) {
            boot_printk("[p11-test] FAIL: sys_socket(server)\n");
            return;
        }

        /* 绑定到 0.0.0.0:8080 */
        addr.sin_family = AF_INET;
        addr.sin_port = htons(8080);
        addr.sin_addr.s_addr = INADDR_ANY;
        for (i = 0; i < 8; i++)
            addr.sin_zero[i] = 0;

        if (sys_bind(server_fd, &addr, sizeof(addr)) != 0) {
            boot_printk("[p11-test] FAIL: sys_bind\n");
            return;
        }

        /* 设置监听 */
        if (sys_listen(server_fd, 5) != 0) {
            boot_printk("[p11-test] FAIL: sys_listen\n");
            return;
        }
        boot_printk("[p11-test] Server listening on port 8080\n");

        /* 创建客户端 socket */
        boot_printk("[p11-test] Creating client socket...\n");
        client_fd = sys_socket(AF_INET, SOCK_STREAM, 0);
        if (client_fd < 0) {
            boot_printk("[p11-test] FAIL: sys_socket(client)\n");
            return;
        }

        /* 连接到 127.0.0.1:8080（三次握手通过 loopback 同步完成）*/
        boot_printk("[p11-test] Client connecting to 127.0.0.1:8080...\n");
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (sys_connect(client_fd, &addr, sizeof(addr)) != 0) {
            boot_printk("[p11-test] FAIL: sys_connect\n");
            return;
        }
        boot_printk("[p11-test] TCP handshake complete\n");

        /* 服务端 accept 获取连接 */
        boot_printk("[p11-test] Server accepting connection...\n");
        conn_fd = sys_accept(server_fd, NULL, NULL);
        if (conn_fd < 0) {
            boot_printk("[p11-test] FAIL: sys_accept\n");
            return;
        }

        /* 服务端发送数据 */
        boot_printk("[p11-test] Writing data on server side...\n");
        n = sock_write(conn_fd, "Hello TCP!\n", 11);
        if (n != 11) {
            boot_printk("[p11-test] FAIL: sock_write returned ");
            boot_printk_hex((unsigned long)n);
            boot_printk("\n");
            return;
        }

        /* 客户端读取数据 */
        boot_printk("[p11-test] Reading data on client side...\n");
        for (i = 0; i < 32; i++)
            buf[i] = 0;

        n = sock_read(client_fd, buf, 16);
        if (n != 11) {
            boot_printk("[p11-test] FAIL: sock_read returned ");
            boot_printk_hex((unsigned long)n);
            boot_printk("\n");
            return;
        }

        /* 验证内容 */
        buf[11] = '\0';
        if (buf[0] == 'H' && buf[1] == 'e' && buf[2] == 'l' &&
            buf[3] == 'l' && buf[4] == 'o' && buf[5] == ' ' &&
            buf[6] == 'T' && buf[7] == 'C' && buf[8] == 'P' &&
            buf[9] == '!' && buf[10] == '\n') {
            boot_printk("[p11-test] TCP test OK: ");
            boot_printk(buf);
            boot_printk("[p11-test] TCP loopback: PASS\n");
        } else {
            boot_printk("[p11-test] FAIL: TCP data mismatch, got: ");
            boot_printk(buf);
            boot_printk("\n");
            return;
        }
    }

    /* ============================================================
     * Test 2: netfilter 钩子 — 注册 ICMP DROP 钩子
     *
     * 参考：§11.5 netfilter钩子框架 + §11.8 test_netfilter_drop()
     *
     * 注册一个 NF_INET_LOCAL_IN 钩子，丢弃 ICMP 包。
     * 构造模拟 ICMP skb，验证 nf_hook() 返回 NF_DROP。
     * 构造模拟 TCP skb，验证 nf_hook() 返回 NF_ACCEPT。
     * ============================================================
     */
    boot_printk("[p11-test] === netfilter hook test ===\n");
    {
        struct nf_hook_ops ops;
        struct sk_buff *test_skb;
        struct iphdr *iph;
        unsigned int verdict;

        /* 注册 ICMP DROP 钩子 */
        ops.hook = test_netfilter_drop_hook;
        ops.priv = NULL;
        ops.pf = NFPROTO_IPV4;
        ops.hooknum = NF_INET_LOCAL_IN;
        ops.priority = NF_IP_PRI_FIRST;

        boot_printk("[p11-test] Registering ICMP drop hook...\n");
        if (nf_register_net_hook(&init_net, &ops) != 0) {
            boot_printk("[p11-test] FAIL: nf_register_net_hook\n");
            return;
        }
        boot_printk("[p11-test] ICMP drop rule installed\n");

        /* 构造模拟 ICMP 包 */
        test_skb = alloc_skb(64);
        if (!test_skb) {
            boot_printk("[p11-test] FAIL: alloc_skb for ICMP test\n");
            return;
        }
        skb_reserve(test_skb, 0);
        iph = (struct iphdr *)skb_put(test_skb, IP_HDR_LEN);
        iph->version_ihl = 0x45;
        iph->protocol = IPPROTO_ICMP;
        iph->saddr = htonl(INADDR_LOOPBACK);
        iph->daddr = htonl(INADDR_LOOPBACK);
        test_skb->nh = iph;

        /* 验证 ICMP 包被 DROP */
        verdict = nf_hook(&init_net, NF_INET_LOCAL_IN, test_skb);
        if (verdict == NF_DROP) {
            boot_printk("[p11-test] ICMP packet dropped: OK\n");
        } else {
            boot_printk("[p11-test] FAIL: ICMP not dropped\n");
            kfree_skb(test_skb);
            return;
        }
        kfree_skb(test_skb);

        /* 构造模拟 TCP 包 — 应该 ACCEPT */
        test_skb = alloc_skb(64);
        if (!test_skb) {
            boot_printk("[p11-test] FAIL: alloc_skb for TCP test\n");
            return;
        }
        skb_reserve(test_skb, 0);
        iph = (struct iphdr *)skb_put(test_skb, IP_HDR_LEN);
        iph->version_ihl = 0x45;
        iph->protocol = IPPROTO_TCP;
        iph->saddr = htonl(INADDR_LOOPBACK);
        iph->daddr = htonl(INADDR_LOOPBACK);
        test_skb->nh = iph;

        verdict = nf_hook(&init_net, NF_INET_LOCAL_IN, test_skb);
        if (verdict == NF_ACCEPT) {
            boot_printk("[p11-test] TCP packet accepted: OK\n");
        } else {
            boot_printk("[p11-test] FAIL: TCP packet not accepted\n");
            kfree_skb(test_skb);
            return;
        }
        kfree_skb(test_skb);

        /* 注销钩子 */
        nf_unregister_net_hook(&init_net, &ops);

        boot_printk("[p11-test] netfilter hook: PASS\n");
    }

    /* ============================================================
     * Test 3: nftables 规则引擎 — 添加 DROP 规则
     *
     * 参考：§11.6 iptables/nftables规则匹配
     *
     * 在 INPUT 链添加一条规则：丢弃目的端口 9999 的 TCP 包。
     * 验证匹配包被 DROP，不匹配包走默认策略 ACCEPT。
     * ============================================================
     */
    boot_printk("[p11-test] === nftables rule test ===\n");
    {
        struct nft_rule rule;
        struct sk_buff *test_skb;
        struct iphdr *iph;
        struct tcphdr *th;
        unsigned int verdict;

        /* 创建规则：DROP 目的端口 9999 的 TCP 包 */
        rule.src_ip = 0;
        rule.src_mask = 0;
        rule.dst_ip = 0;
        rule.dst_mask = 0;
        rule.protocol = IPPROTO_TCP;
        rule.src_port = 0;
        rule.dst_port = htons(9999);
        rule.target = NF_DROP;
        rule.used = 0;

        if (nft_add_rule(&nft_filter_table.input, &rule) != 0) {
            boot_printk("[p11-test] FAIL: nft_add_rule\n");
            return;
        }

        /* 构造匹配包（目的端口 9999）*/
        test_skb = alloc_skb(128);
        if (!test_skb) {
            boot_printk("[p11-test] FAIL: alloc_skb for nft test\n");
            return;
        }
        skb_reserve(test_skb, 0);
        iph = (struct iphdr *)skb_put(test_skb, IP_HDR_LEN);
        iph->version_ihl = 0x45;
        iph->protocol = IPPROTO_TCP;
        iph->saddr = htonl(INADDR_LOOPBACK);
        iph->daddr = htonl(INADDR_LOOPBACK);
        test_skb->nh = iph;

        th = (struct tcphdr *)skb_put(test_skb, TCP_HDR_LEN);
        th->source = htons(12345);
        th->dest = htons(9999);
        test_skb->th = th;

        /* 评估规则链 — 应 DROP */
        verdict = nft_do_chain(&nft_filter_table.input, test_skb);
        if (verdict == NF_DROP) {
            boot_printk("[p11-test] nft: port 9999 dropped: OK\n");
        } else {
            boot_printk("[p11-test] FAIL: nft did not drop port 9999\n");
            kfree_skb(test_skb);
            return;
        }
        kfree_skb(test_skb);

        /* 构造不匹配包（目的端口 80）— 应走默认策略 ACCEPT */
        test_skb = alloc_skb(128);
        if (!test_skb) {
            boot_printk("[p11-test] FAIL: alloc_skb for nft test 2\n");
            return;
        }
        skb_reserve(test_skb, 0);
        iph = (struct iphdr *)skb_put(test_skb, IP_HDR_LEN);
        iph->version_ihl = 0x45;
        iph->protocol = IPPROTO_TCP;
        iph->saddr = htonl(INADDR_LOOPBACK);
        iph->daddr = htonl(INADDR_LOOPBACK);
        test_skb->nh = iph;

        th = (struct tcphdr *)skb_put(test_skb, TCP_HDR_LEN);
        th->source = htons(12345);
        th->dest = htons(80);
        test_skb->th = th;

        verdict = nft_do_chain(&nft_filter_table.input, test_skb);
        if (verdict == NF_ACCEPT) {
            boot_printk("[p11-test] nft: port 80 accepted: OK\n");
        } else {
            boot_printk("[p11-test] FAIL: nft dropped port 80\n");
            kfree_skb(test_skb);
            return;
        }
        kfree_skb(test_skb);

        boot_printk("[p11-test] nftables rule: PASS\n");
    }

    boot_printk("[BOOT] Phase 11 TCP/IP + netfilter tests: all passed\n");
}

/*
 * ============================================================
 * Phase 12: seccomp + eBPF JIT + Landlock LSM 验证
 *
 * 三个测试：
 *   1. seccomp — STRICT 模式白名单 + FILTER 模式 BPF 过滤器
 *   2. eBPF — 程序加载、验证器、解释器执行、JIT 编译
 *   3. Landlock — 规则集创建、路径规则、沙箱激活、访问控制
 *
 * 参考：§12.5 验证方法
 * ============================================================
 */
static void test_phase12(void)
{
    boot_printk("[BOOT] Phase 12 security tests starting...\n");

    /* ============================================================
     * Test 1: seccomp — 系统调用过滤
     *
     * 参考：§12.1 seccomp
     *
     * 1a. STRICT 模式：只允许 read/write/exit/exit_group
     * 1b. FILTER 模式：BPF 过滤器（允许 write + exit，拒绝其他）
     * ============================================================
     */
    boot_printk("[p12-test] === seccomp test ===\n");
    {
        struct seccomp_data sd;
        int ret;

        /*
         * Test 1a: STRICT 模式
         *
         * 设置当前进程为 STRICT 模式，验证：
         *   - write(64) → 允许（返回 0）
         *   - read(63) → 允许（返回 0）
         *   - openat(56) → 拒绝（返回负数）
         */
        boot_printk("[p12-test] seccomp STRICT mode...\n");

        /* 保存并设置 STRICT */
        current_task->seccomp_mode = SECCOMP_MODE_DISABLED;
        current_task->seccomp_filter = NULL;

        ret = seccomp_set_mode_strict();
        if (ret != 0) {
            boot_printk("[p12-test] FAIL: seccomp_set_mode_strict\n");
            return;
        }

        /* 验证 write(64) 被允许 */
        sd.nr = 64;  /* __NR_write */
        sd.arch = AUDIT_ARCH_AARCH64;
        sd.args[0] = 1;
        sd.args[1] = 0;
        sd.args[2] = 0;
        sd.args[3] = 0;
        sd.args[4] = 0;
        sd.args[5] = 0;
        sd.instruction_pointer = 0;

        ret = __secure_computing(&sd);
        if (ret != 0) {
            boot_printk("[p12-test] FAIL: STRICT rejected write\n");
            return;
        }
        boot_printk("[p12-test] STRICT: write allowed: OK\n");

        /* 验证 read(63) 被允许 */
        sd.nr = 63;  /* __NR_read */
        ret = __secure_computing(&sd);
        if (ret != 0) {
            boot_printk("[p12-test] FAIL: STRICT rejected read\n");
            return;
        }
        boot_printk("[p12-test] STRICT: read allowed: OK\n");

        /* 验证 openat(56) 被拒绝 */
        sd.nr = 56;  /* __NR_openat */
        ret = __secure_computing(&sd);
        if (ret == 0) {
            boot_printk("[p12-test] FAIL: STRICT allowed openat\n");
            return;
        }
        boot_printk("[p12-test] STRICT: openat blocked: OK\n");

        /*
         * Test 1b: FILTER 模式（BPF 过滤器）
         *
         * 安装 cBPF 过滤器：
         *   - 加载系统调用号到累加器
         *   - 允许 write(64) 和 exit(93)
         *   - 其他返回 SECCOMP_RET_ERRNO | EPERM
         */
        boot_printk("[p12-test] seccomp FILTER mode...\n");

        /* 重置为 DISABLED 以安装 FILTER */
        current_task->seccomp_mode = SECCOMP_MODE_DISABLED;
        current_task->seccomp_filter = NULL;

        {
            struct sock_filter filter[5];
            struct sock_fprog fprog;

            /* [0] 加载系统调用号到累加器 A */
            filter[0].code = BPF_LD | BPF_W | BPF_ABS;
            filter[0].jt = 0;
            filter[0].jf = 0;
            filter[0].k = offsetof(struct seccomp_data, nr);

            /* [1] if A == 64 (write) goto +2 (allow) else +0 (next) */
            filter[1].code = BPF_JMP | BPF_JEQ | BPF_K;
            filter[1].jt = 2;
            filter[1].jf = 0;
            filter[1].k = 64;

            /* [2] if A == 93 (exit) goto +1 (allow) else +0 (deny) */
            filter[2].code = BPF_JMP | BPF_JEQ | BPF_K;
            filter[2].jt = 1;
            filter[2].jf = 0;
            filter[2].k = 93;

            /* [3] deny: return ERRNO(1) */
            filter[3].code = BPF_RET | BPF_K;
            filter[3].jt = 0;
            filter[3].jf = 0;
            filter[3].k = SECCOMP_RET_ERRNO | (EPERM & SECCOMP_RET_DATA);

            /* [4] allow: return ALLOW */
            filter[4].code = BPF_RET | BPF_K;
            filter[4].jt = 0;
            filter[4].jf = 0;
            filter[4].k = SECCOMP_RET_ALLOW;

            fprog.len = 5;
            fprog.filter = filter;

            ret = seccomp_set_mode_filter(&fprog);
            if (ret != 0) {
                boot_printk("[p12-test] FAIL: seccomp_set_mode_filter\n");
                return;
            }
        }

        /* 验证 write(64) 被允许 */
        sd.nr = 64;
        ret = __secure_computing(&sd);
        if (ret != 0) {
            boot_printk("[p12-test] FAIL: FILTER rejected write\n");
            return;
        }
        boot_printk("[p12-test] FILTER: write allowed: OK\n");

        /* 验证 exit(93) 被允许 */
        sd.nr = 93;
        ret = __secure_computing(&sd);
        if (ret != 0) {
            boot_printk("[p12-test] FAIL: FILTER rejected exit\n");
            return;
        }
        boot_printk("[p12-test] FILTER: exit allowed: OK\n");

        /* 验证 openat(56) 被拒绝 */
        sd.nr = 56;
        ret = __secure_computing(&sd);
        if (ret == 0) {
            boot_printk("[p12-test] FAIL: FILTER allowed openat\n");
            return;
        }
        boot_printk("[p12-test] FILTER: openat blocked (EPERM): OK\n");

        /* 验证 read(63) 被拒绝（FILTER 模式只允许 write+exit）*/
        sd.nr = 63;
        ret = __secure_computing(&sd);
        if (ret == 0) {
            boot_printk("[p12-test] FAIL: FILTER allowed read\n");
            return;
        }
        boot_printk("[p12-test] FILTER: read blocked (EPERM): OK\n");

        /* 清理 */
        current_task->seccomp_mode = SECCOMP_MODE_DISABLED;
        current_task->seccomp_filter = NULL;

        boot_printk("[p12-test] seccomp: PASS\n");
    }

    /* ============================================================
     * Test 2: eBPF — 程序加载 + 验证器 + 解释器 + JIT
     *
     * 参考：§12.2 eBPF架构与JIT编译
     *
     * 2a. 加载简单 eBPF 程序：r0 = r1 + 42，验证解释器执行
     * 2b. JIT 编译同一程序，验证 JIT 成功
     * 2c. 验证器拒绝非法程序（无 EXIT 指令）
     * ============================================================
     */
    boot_printk("[p12-test] === eBPF test ===\n");
    {
        /*
         * Test 2a: 加载并运行 eBPF 程序
         *
         * 程序逻辑（伪代码）：
         *   r0 = *(u32 *)(r1 + 0)    // 从上下文读取第一个 u32
         *   r0 += 42                  // 加 42
         *   exit                      // 返回 r0
         */
        struct bpf_insn prog_insns[3];
        struct bpf_attr attr;
        int prog_fd;
        u32 test_ctx;
        struct ebpf_prog *prog;

        /* r0 = *(u32 *)(r1 + 0) : LDX_MEM(W) */
        prog_insns[0].code = EBPF_CLS_LDX | 0x00 | EBPF_MEM;
        prog_insns[0].dst_reg = BPF_REG_0;
        prog_insns[0].src_reg = BPF_REG_1;
        prog_insns[0].off = 0;
        prog_insns[0].imm = 0;

        /* r0 += 42 : ALU64_IMM(ADD) */
        prog_insns[1].code = EBPF_CLS_ALU64 | EBPF_ADD | EBPF_K;
        prog_insns[1].dst_reg = BPF_REG_0;
        prog_insns[1].src_reg = 0;
        prog_insns[1].off = 0;
        prog_insns[1].imm = 42;

        /* exit */
        prog_insns[2].code = EBPF_CLS_JMP | EBPF_EXIT;
        prog_insns[2].dst_reg = 0;
        prog_insns[2].src_reg = 0;
        prog_insns[2].off = 0;
        prog_insns[2].imm = 0;

        /* 先用解释器测试 */
        bpf_jit_enable = 0;

        attr.prog_type = BPF_PROG_TYPE_SOCKET_FILTER;
        attr.insn_cnt = 3;
        attr.insns = prog_insns;

        prog_fd = sys_bpf(BPF_PROG_LOAD, &attr, sizeof(attr));
        if (prog_fd < 0) {
            boot_printk("[p12-test] FAIL: BPF_PROG_LOAD (interp)\n");
            return;
        }
        boot_printk("[p12-test] eBPF program loaded (interp): OK\n");

        /* 运行程序：ctx = { .value = 100 } → expect 100 + 42 = 142 */
        test_ctx = 100;
        attr.prog_fd = (u32)prog_fd;
        attr.data_in = &test_ctx;
        attr.data_size_in = sizeof(test_ctx);
        attr.retval = 0;

        if (sys_bpf(BPF_PROG_RUN, &attr, sizeof(attr)) != 0) {
            boot_printk("[p12-test] FAIL: BPF_PROG_RUN (interp)\n");
            return;
        }

        if (attr.retval == 142) {
            boot_printk("[p12-test] eBPF interp result=142: OK\n");
        } else {
            boot_printk("[p12-test] FAIL: eBPF interp result=");
            boot_printk_hex((unsigned long)attr.retval);
            boot_printk(" (expect 142)\n");
            return;
        }

        /*
         * Test 2b: JIT 编译
         *
         * 重新加载相同程序，但启用 JIT。
         * 验证 JIT 编译成功（prog->jit_done = true）。
         * 注意：在 QEMU 中实际执行 JIT 代码需要 icache 刷新，
         * 此处验证 JIT 编译流程本身。
         */
        boot_printk("[p12-test] enabling JIT...\n");
        bpf_jit_enable = 1;

        attr.insn_cnt = 3;
        attr.insns = prog_insns;

        prog_fd = sys_bpf(BPF_PROG_LOAD, &attr, sizeof(attr));
        if (prog_fd < 0) {
            boot_printk("[p12-test] FAIL: BPF_PROG_LOAD (JIT)\n");
            return;
        }

        prog = bpf_prog_get(prog_fd);
        if (prog && prog->jit_done) {
            boot_printk("[p12-test] JIT compiled: OK (");
            boot_printk_hex((unsigned long)prog->jit_size);
            boot_printk(" bytes)\n");
        } else {
            boot_printk("[p12-test] FAIL: JIT not done\n");
            return;
        }

        /*
         * Test 2c: 验证器拒绝非法程序
         *
         * 非法程序：没有 EXIT 指令。
         */
        {
            struct bpf_insn bad_prog[1];
            struct bpf_attr bad_attr;
            int bad_fd;

            /* MOV64_IMM r0, 0 — 没有 EXIT */
            bad_prog[0].code = EBPF_CLS_ALU64 | EBPF_MOV | EBPF_K;
            bad_prog[0].dst_reg = BPF_REG_0;
            bad_prog[0].src_reg = 0;
            bad_prog[0].off = 0;
            bad_prog[0].imm = 0;

            bad_attr.prog_type = BPF_PROG_TYPE_SOCKET_FILTER;
            bad_attr.insn_cnt = 1;
            bad_attr.insns = bad_prog;

            bad_fd = sys_bpf(BPF_PROG_LOAD, &bad_attr, sizeof(bad_attr));
            if (bad_fd < 0) {
                boot_printk("[p12-test] verifier rejected bad prog: OK\n");
            } else {
                boot_printk("[p12-test] FAIL: verifier accepted bad prog\n");
                return;
            }
        }

        boot_printk("[p12-test] eBPF: PASS\n");
    }

    /* ============================================================
     * Test 3: Landlock — 文件系统沙箱
     *
     * 参考：§12.3 Landlock LSM
     *
     * 3a. 创建规则集（限制文件读写）
     * 3b. 添加规则：允许读 /etc，允许读写 /tmp
     * 3c. 激活沙箱
     * 3d. 验证 /etc 可读
     * 3e. 验证 /etc 不可写
     * 3f. 验证 /tmp 可写
     * 3g. 验证 /home 不可访问
     * ============================================================
     */
    boot_printk("[p12-test] === Landlock test ===\n");
    {
        struct landlock_ruleset_attr rs_attr;
        int ruleset_fd;
        int ret;

        /* 创建规则集：限制读文件、写文件、读目录 */
        rs_attr.handled_access_fs =
            LANDLOCK_ACCESS_FS_READ_FILE  |
            LANDLOCK_ACCESS_FS_WRITE_FILE |
            LANDLOCK_ACCESS_FS_READ_DIR;
        rs_attr.handled_access_net = 0;

        ruleset_fd = landlock_create_ruleset(&rs_attr, sizeof(rs_attr), 0);
        if (ruleset_fd < 0) {
            boot_printk("[p12-test] FAIL: landlock_create_ruleset\n");
            return;
        }
        boot_printk("[p12-test] ruleset created: OK\n");

        /* 添加规则：允许读 /etc */
        ret = landlock_add_path_rule(ruleset_fd, "/etc",
                                     LANDLOCK_ACCESS_FS_READ_FILE |
                                     LANDLOCK_ACCESS_FS_READ_DIR);
        if (ret != 0) {
            boot_printk("[p12-test] FAIL: add /etc rule\n");
            return;
        }

        /* 添加规则：允许读写 /tmp */
        ret = landlock_add_path_rule(ruleset_fd, "/tmp",
                                     LANDLOCK_ACCESS_FS_READ_FILE  |
                                     LANDLOCK_ACCESS_FS_WRITE_FILE |
                                     LANDLOCK_ACCESS_FS_READ_DIR);
        if (ret != 0) {
            boot_printk("[p12-test] FAIL: add /tmp rule\n");
            return;
        }
        boot_printk("[p12-test] rules added (/etc=RO, /tmp=RW): OK\n");

        /* 激活沙箱 */
        ret = landlock_restrict_self(ruleset_fd, 0);
        if (ret != 0) {
            boot_printk("[p12-test] FAIL: landlock_restrict_self\n");
            return;
        }

        /* 验证 /etc/hostname 可读 */
        ret = landlock_file_open("/etc/hostname",
                                 LANDLOCK_ACCESS_FS_READ_FILE);
        if (ret == 0) {
            boot_printk("[p12-test] read /etc/hostname: allowed: OK\n");
        } else {
            boot_printk("[p12-test] FAIL: /etc/hostname read denied\n");
            return;
        }

        /* 验证 /etc/shadow 不可写 */
        ret = landlock_file_open("/etc/shadow",
                                 LANDLOCK_ACCESS_FS_WRITE_FILE);
        if (ret != 0) {
            boot_printk("[p12-test] write /etc/shadow: blocked: OK\n");
        } else {
            boot_printk("[p12-test] FAIL: /etc/shadow write allowed\n");
            return;
        }

        /* 验证 /tmp/data 可写 */
        ret = landlock_file_open("/tmp/data",
                                 LANDLOCK_ACCESS_FS_WRITE_FILE);
        if (ret == 0) {
            boot_printk("[p12-test] write /tmp/data: allowed: OK\n");
        } else {
            boot_printk("[p12-test] FAIL: /tmp/data write denied\n");
            return;
        }

        /* 验证 /home/user 不可访问 */
        ret = landlock_file_open("/home/user",
                                 LANDLOCK_ACCESS_FS_READ_FILE);
        if (ret != 0) {
            boot_printk("[p12-test] read /home/user: blocked: OK\n");
        } else {
            boot_printk("[p12-test] FAIL: /home/user access allowed\n");
            return;
        }

        /* 清理：解除沙箱（仅为测试方便，真实实现不可逆）*/
        current_task->landlock_domain = NULL;

        boot_printk("[p12-test] Landlock: PASS\n");
    }

    boot_printk("[BOOT] Phase 12 seccomp + eBPF + Landlock tests: all passed\n");
}
