/* SPDX-License-Identifier: GPL-2.0 */
/*
 * arm64os/arch/arm64/mm/mmu.c
 *
 * MMU 初始化：早期页表建立
 *
 * 参考：arch/arm64/mm/mmu.c
 *       arch/arm64/kernel/head.S（__create_page_tables）
 *
 * Phase 2 实现策略（简化恒等映射）：
 *
 * Phase 5 新增：
 *   - create_user_pgd()：创建用户进程页表（包含内核恒等映射 + 用户页）
 *   - map_user_page()：在用户页表中映射单个 4KB 页
 *   - 用户虚拟地址布局：
 *     代码段 0x00400000, 用户栈顶 0x00800000
 *
 *
 *   目标：让 MMU 开启后内核能继续正常运行。
 *
 *   方法：建立 TTBR0 恒等映射（VA == PA）：
 *     物理地址 [0x40000000, 0x80000000) → 虚拟地址 [0x40000000, 0x80000000)
 *     使用 2 个 L1（PUD）1GB Block Descriptor，覆盖整个 QEMU virt 内存。
 *
 *   页表结构（4KB粒度，48位VA，T0SZ=16）：
 *     TTBR0 → init_pgd[512]（L0，PGD）
 *       init_pgd[0] → init_pud[512]（L1，PUD）
 *         init_pud[1] → 1GB Block Descriptor @ PA 0x40000000
 *
 *   （QEMU virt 物理内存 1GB 位于 0x40000000-0x7FFFFFFF）
 *
 * 调用流程：
 *   start_kernel()
 *     → mmu_init()
 *         → create_page_tables()  [本文件]
 *         → cpu_init()            [proc.S：设置MAIR/TCR]
 *         → enable_mmu(pgd_phys)  [proc.S：写TTBR0，置SCTLR.M]
 */

#include <linux/types.h>
#include <asm/memory.h>
#include <asm/pgtable.h>

/*
 * ============================================================
 * 静态早期页表（存放在 .bss，由 head.S 清零初始化）
 *
 * 这两个数组就是 Phase 2 的全部页表内容：
 *   init_pgd：L0 页全局目录（PGD），512个条目 × 8字节 = 4KB
 *   init_pud：L1 页上级目录（PUD），512个条目 × 8字节 = 4KB
 *
 * __attribute__((aligned(4096)))：必须4KB对齐，因为TTBR0/TTBR1
 * 要求页表基址4KB对齐（低12位由硬件忽略/用于其他用途）。
 * ============================================================
 */
static unsigned long init_pgd[PTRS_PER_PGD] __attribute__((aligned(4096)));
static unsigned long init_pud[PTRS_PER_PUD] __attribute__((aligned(4096)));

/*
 * 由 proc.S 提供，声明在此处以供调用
 */
extern void cpu_init(void);
extern void enable_mmu(unsigned long pgd_phys);

/*
 * ============================================================
 * create_page_tables - 建立恒等映射页表
 *
 * 在 MMU 开启之前调用（物理地址运行模式）。
 * 此时VA == PA（无翻译），所以指针就是物理地址。
 *
 * 映射内容：
 *   TTBR0: 恒等映射 [0x40000000, 0x80000000)
 *          使用 L1 Block Descriptor（1GB大页）
 *
 * 返回值：init_pgd 的物理地址（用于写入 TTBR0_EL1）
 * ============================================================
 */
static unsigned long create_page_tables(void)
{
    unsigned long pgd_phys = (unsigned long)init_pgd;
    unsigned long pud_phys = (unsigned long)init_pud;

    /*
     * Step 1a: 构建 L1[0]：设备内存区域 [0x00000000, 0x40000000)
     *
     * QEMU virt machine 中第一个 1GB 是设备 MMIO 区域，包括：
     *   0x09000000: PL011 UART（内核启动时串口输出必须用到）
     *   0x08000000: GIC v3 分发器
     *   0x0a000000: VirtIO 设备
     *
     * 使用 MT_DEVICE_nGnRnE（index 0）属性：
     *   - 严格顺序访问（no Gather/Reorder/Early-write-ack）
     *   - 禁止执行（PXN + UXN）
     *   - 不可缓存
     *
     * MMU 开启后 boot_printk() 需要写 UART，必须先映射此区域，
     * 否则第一次 printk 就会触发 Translation Fault → 死循环。
     */
    init_pud[pud_index(0x00000000UL)] =
        mk_block_desc(0x00000000UL,
                      PD_ATTRINDX(0) |   /* MT_DEVICE_nGnRnE */
                      PD_SH_OUTER     |  /* Outer Shareable（设备内存标准配置）*/
                      PD_AF           |  /* Access Flag */
                      PD_PXN          |  /* 特权态不可执行 */
                      PD_UXN);           /* 用户态不可执行 */

    /*
     * Step 1b: 构建 L1[1]：RAM 区域 [0x40000000, 0x80000000)
     *
     * QEMU virt machine 将 1GB RAM 放在 0x40000000。
     * 0x40000000 >> 30 = 1，所以 init_pud[1] 是正确的 L1 索引。
     *
     * 使用 MT_NORMAL（index 3）属性：
     *   - 写回可缓存（Write-Back Read/Write-Allocate）
     *   - Inner Shareable（对 SMP 广播缓存一致性操作）
     *
     * 注意：1GB Block Descriptor 中 bits[29:12] 必须为零（MBZ）。
     * 0x40000000 的 bits[29:12] 全为零，满足此要求。
     */
    init_pud[pud_index(0x40000000UL)] =
        mk_block_desc(0x40000000UL,
                      PD_ATTRINDX(3) |   /* MT_NORMAL (Write-Back) */
                      PD_SH_INNER     |  /* Inner Shareable */
                      PD_AF);            /* Access Flag = 已访问 */

    /*
     * Step 2: 构建 L0（PGD）条目，指向 L1 表
     *
     * VA[47:39] = 0 → init_pgd[0] 覆盖低 512GB（0x0 - 0x7FFFFFFFFFFF）。
     * Table Descriptor（L0条目）格式：
     *   bits[1:0]  = 0b11（PD_TABLE：Table descriptor）
     *   bits[47:12]= 下级页表物理地址（4KB对齐，低12位为0）
     */
    init_pgd[pgd_index(0x40000000UL)] =
        mk_table_desc(pud_phys);

    /*
     * dsb ish：Data Synchronization Barrier
     * 确保页表写入对内存系统和页表遍历硬件可见，
     * 然后再写入 TTBR0_EL1（在 enable_mmu 中执行）。
     */
    __asm__ volatile("dsb ish" ::: "memory");

    return pgd_phys;
}

/*
 * ============================================================
 * mmu_init - MMU 初始化入口（由 start_kernel 调用）
 *
 * 操作顺序：
 *   1. 建立恒等映射页表
 *   2. 配置 CPU 内存属性和地址翻译控制（MAIR_EL1, TCR_EL1）
 *   3. 写入 TTBR0_EL1，开启 MMU（SCTLR_EL1.M = 1）
 *
 * 返回后：CPU 开始通过页表进行地址翻译，但由于恒等映射，
 * 所有当前使用的地址（内核代码/数据/栈）翻译结果不变。
 * ============================================================
 */
void mmu_init(void)
{
    unsigned long pgd_phys;

    /* Step 1: 建立页表（必须在 cpu_init 之前，因为此时还在裸机模式）*/
    pgd_phys = create_page_tables();

    /* Step 2: 设置 MAIR_EL1 和 TCR_EL1 */
    cpu_init();

    /* Step 3: 写 TTBR0_EL1，开启 MMU */
    enable_mmu(pgd_phys);

    /*
     * 此处 MMU 已开启，运行在恒等映射（VA == PA）下。
     * 内核的所有全局变量、函数指针、栈地址均有效。
     */
}

/*
 * ============================================================
 * Phase 5：用户进程页表支持
 * ============================================================
 */

/* 外部：Buddy 分配器 */
struct page;
struct page *alloc_pages(unsigned int order);
void *page_address(struct page *page);

/* 外部：printk */
void boot_printk(const char *s);
void boot_printk_hex(unsigned long val);

/*
 * alloc_page_table - 分配一个 4KB 的零初始化页表页
 *
 * 从 Buddy 分配器获取 order=0（4KB）页，手动清零。
 * 返回物理地址（恒等映射下 == 虚拟地址）。
 */
static unsigned long alloc_page_table(void)
{
    struct page *pg = alloc_pages(0);
    unsigned long *table;
    int i;

    if (!pg)
        return 0;

    table = (unsigned long *)page_address(pg);

    /* 清零：所有条目初始为 PD_INVALID (0) */
    for (i = 0; i < 512; i++)
        table[i] = 0;

    return (unsigned long)table;
}

/*
 * create_user_pgd - 创建用户进程的 TTBR0 页表
 *
 * 分配 L0(PGD) + L1(PUD) 页表，填入内核恒等映射：
 *   L1[0] = 1GB Block → [0x0, 0x40000000) 设备内存（AP=00，仅内核）
 *   L1[1] = 1GB Block → [0x40000000, 0x80000000) RAM（AP=00，仅内核）
 *
 * 用户页面通过后续 map_user_page() 单独添加。
 * 当用户 VA 在第一个 1GB 范围内时，L1[0] 会被替换为 L2 table。
 *
 * 返回：PGD 物理地址（用于写入 TTBR0_EL1）。0 表示失败。
 */
unsigned long create_user_pgd(void)
{
    unsigned long pgd_phys, pud_phys;
    unsigned long *pgd, *pud;

    pgd_phys = alloc_page_table();
    if (!pgd_phys) return 0;

    pud_phys = alloc_page_table();
    if (!pud_phys) return 0;

    pgd = (unsigned long *)pgd_phys;
    pud = (unsigned long *)pud_phys;

    /* 内核恒等映射（与 init 页表相同，但 AP=00 仅内核可访问）*/
    /* L1[0]: 设备内存 [0x0, 0x40000000) */
    pud[pud_index(0x00000000UL)] =
        mk_block_desc(0x00000000UL,
                      PD_ATTRINDX(0) | PD_SH_OUTER | PD_AF |
                      PD_PXN | PD_UXN);

    /* L1[1]: RAM [0x40000000, 0x80000000) */
    pud[pud_index(0x40000000UL)] =
        mk_block_desc(0x40000000UL,
                      PD_ATTRINDX(3) | PD_SH_INNER | PD_AF |
                      PD_UXN);

    /* L0[0] → PUD */
    pgd[pgd_index(0x00000000UL)] = mk_table_desc(pud_phys);

    __asm__ volatile("dsb ish" ::: "memory");

    return pgd_phys;
}

/*
 * map_user_page - 在用户页表中映射单个 4KB 页
 *
 * @pgd_phys: 用户 PGD 物理地址（create_user_pgd 返回值）
 * @va:       用户虚拟地址（必须 4KB 对齐）
 * @pa:       物理地址（必须 4KB 对齐）
 * @attrs:    页属性（PD_USER_EXEC 或 PD_USER_DATA）
 *
 * 自动创建中间级页表（L1→L2→L3）。
 * 如果 VA 在第一个 1GB 范围（0x0-0x3FFFFFFF）内，
 * 将 L1[0] 的 1GB block 替换为 L2 table，并按需映射设备 MMIO。
 *
 * 返回：0 成功，-1 失败。
 */
int map_user_page(unsigned long pgd_phys, unsigned long va,
                  unsigned long pa, unsigned long attrs)
{
    unsigned long *pgd = (unsigned long *)pgd_phys;
    unsigned long *pud, *pmd, *pte;
    unsigned long pud_phys, pmd_phys, pte_phys;
    unsigned int l0_idx, l1_idx, l2_idx, l3_idx;

    l0_idx = pgd_index(va);
    l1_idx = pud_index(va);
    l2_idx = pmd_index(va);
    l3_idx = pte_index(va);

    /* L0 → L1 */
    if (!pte_valid(pgd[l0_idx])) {
        pud_phys = alloc_page_table();
        if (!pud_phys) return -1;
        pgd[l0_idx] = mk_table_desc(pud_phys);
    }
    pud = (unsigned long *)table_phys(pgd[l0_idx]);

    /* L1 → L2：如果当前是 1GB block，需要拆分 */
    if (pte_is_block(pud[l1_idx])) {
        /*
         * 拆分 1GB block → L2 table。
         * 对于设备内存区 (L1[0])，我们需要在 L2 中映射关键设备 MMIO。
         */
        unsigned long old_block_pa = pud[l1_idx] & ~((1UL << PUD_SHIFT) - 1);
        unsigned long old_attrs_raw = pud[l1_idx] & ((1UL << PUD_SHIFT) - 1);
        /* 提取属性（去掉 type bits [1:0]）*/
        unsigned long block_attrs = old_attrs_raw & ~3UL;

        pmd_phys = alloc_page_table();
        if (!pmd_phys) return -1;
        pmd = (unsigned long *)pmd_phys;

        /*
         * 将原 1GB block 拆分为 512 个 2MB block（保留原属性）。
         */
        {
            int i;
            for (i = 0; i < 512; i++) {
                pmd[i] = mk_block_desc_2m(
                    old_block_pa + ((unsigned long)i << PMD_SHIFT),
                    block_attrs);
            }
        }

        /* 替换 L1 entry */
        pud[l1_idx] = mk_table_desc(pmd_phys);
    } else if (!pte_valid(pud[l1_idx])) {
        pmd_phys = alloc_page_table();
        if (!pmd_phys) return -1;
        pud[l1_idx] = mk_table_desc(pmd_phys);
    }
    pmd = (unsigned long *)table_phys(pud[l1_idx]);

    /* L2 → L3：如果当前是 2MB block，需要拆分 */
    if (pte_is_block(pmd[l2_idx])) {
        unsigned long old_block_pa = pmd[l2_idx] & ~((1UL << PMD_SHIFT) - 1);
        unsigned long old_attrs_raw = pmd[l2_idx] & ((1UL << PMD_SHIFT) - 1);
        unsigned long block_attrs = old_attrs_raw & ~3UL;

        pte_phys = alloc_page_table();
        if (!pte_phys) return -1;
        pte = (unsigned long *)pte_phys;

        /* 将 2MB block 拆分为 512 个 4KB page */
        {
            int i;
            for (i = 0; i < 512; i++) {
                pte[i] = mk_page_desc(
                    old_block_pa + ((unsigned long)i << PAGE_SHIFT),
                    block_attrs);
            }
        }

        pmd[l2_idx] = mk_table_desc(pte_phys);
    } else if (!pte_valid(pmd[l2_idx])) {
        pte_phys = alloc_page_table();
        if (!pte_phys) return -1;
        pmd[l2_idx] = mk_table_desc(pte_phys);
    }
    pte = (unsigned long *)table_phys(pmd[l2_idx]);

    /* L3 页表项 */
    pte[l3_idx] = mk_page_desc(pa, attrs);

    __asm__ volatile("dsb ish" ::: "memory");
    /* TLB invalidate for this VA */
    __asm__ volatile("tlbi vale1is, %0" :: "r"(va >> PAGE_SHIFT) : "memory");
    __asm__ volatile("dsb ish; isb" ::: "memory");

    return 0;
}

/*
 * switch_ttbr0 - 切换 TTBR0_EL1 到指定页表
 *
 * @pgd_phys: 新的 PGD 物理地址
 *
 * 用于进程上下文切换时更新用户空间页表。
 * 包含 DSB + ISB 以确保切换完成。
 */
void switch_ttbr0(unsigned long pgd_phys)
{
    __asm__ volatile(
        "dsb ish\n"
        "msr ttbr0_el1, %0\n"
        "isb\n"
        "tlbi vmalle1is\n"     /* 简化：全 TLB 无效化 */
        "dsb ish\n"
        "isb\n"
        :: "r"(pgd_phys) : "memory"
    );
}

/*
 * get_kernel_pgd - 获取内核初始页表物理地址
 *
 * 用于内核线程上下文切换时恢复 TTBR0。
 */
unsigned long get_kernel_pgd(void)
{
    return (unsigned long)init_pgd;
}
