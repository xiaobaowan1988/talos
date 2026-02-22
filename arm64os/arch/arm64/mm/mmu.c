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
     * Step 1: 构建 L1（PUD）恒等映射条目
     *
     * QEMU virt machine 将 1GB RAM 放在 0x40000000。
     * 0x40000000 >> 30 = 1，所以 init_pud[1] 是正确的 L1 索引。
     *
     * 1GB Block Descriptor 格式（L1 Block）：
     *   bits[1:0]  = 0b01（PD_BLOCK：Block descriptor at L1）
     *   bits[4:2]  = 0b011（AttrIdx=3：MT_NORMAL，写回可缓存内存）
     *   bits[9:8]  = 0b11（SH=Inner Shareable）
     *   bit [10]   = 1（AF：Access Flag，避免Access Fault）
     *   bits[47:30]= 0x40000000（1GB对齐的物理基址）
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
