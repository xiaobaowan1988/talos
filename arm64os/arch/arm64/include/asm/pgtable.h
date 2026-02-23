/* SPDX-License-Identifier: GPL-2.0 */
/*
 * arm64os/arch/arm64/include/asm/pgtable.h
 *
 * ARMv8-A 页表项（Page Table Entry）格式定义
 *
 * 参考：arch/arm64/include/asm/pgtable-hwdef.h
 *       arch/arm64/include/asm/pgtable-prot.h
 *       ARMv8-A ARM (DDI 0487) Section D5
 *
 * 支持 4KB 粒度，48位虚拟地址，4级页表（L0-L3）：
 *   L0 (PGD): 512GB/entry，只能是 Table descriptor
 *   L1 (PUD): 1GB/entry，可以是 Block descriptor 或 Table descriptor
 *   L2 (PMD): 2MB/entry，可以是 Block descriptor 或 Table descriptor
 *   L3 (PTE): 4KB/entry，必须是 Page descriptor
 */

#ifndef __ASM_PGTABLE_H
#define __ASM_PGTABLE_H

#include <asm/memory.h>

/*
 * ============================================================
 * 描述符类型位 [1:0]
 * ============================================================
 */
#define PD_INVALID          0x0UL   /* 无效描述符（触发Translation Fault）*/
#define PD_BLOCK            0x1UL   /* Block descriptor（L1=1GB, L2=2MB）*/
#define PD_TABLE            0x3UL   /* Table descriptor（指向下级页表）*/
#define PD_PAGE             0x3UL   /* Page descriptor（L3专用）*/
/* 注：L3的Page descriptor和Table descriptor编码相同（bit[1]=1, bit[0]=1），
 *     区别在于层级：L3只能是Page descriptor */

/*
 * ============================================================
 * 低位属性位（Lower Page/Block Attributes）
 * ============================================================
 */

/* [4:2] AttrIdx：MAIR_EL1 索引，选择内存属性 */
#define PD_ATTRINDX(n)      (((unsigned long)(n)) << 2)
/* 对应 sysreg.h 中定义的 MT_* 宏 */

/* [5] NS：Non-Secure（对EL3无关，EL1下通常为0）*/
#define PD_NS               (1UL << 5)

/* [7:6] AP[2:1]：访问权限
 *   AP=00: EL1读写，EL0无权
 *   AP=01: EL1/EL0均可读写
 *   AP=10: EL1只读，EL0无权
 *   AP=11: EL1/EL0均只读
 */
#define PD_AP_RW_EL1        (0UL << 6)   /* 内核读写，用户无权 */
#define PD_AP_RW_ALL        (1UL << 6)   /* 内核/用户读写 */
#define PD_AP_RO_EL1        (2UL << 6)   /* 内核只读，用户无权 */
#define PD_AP_RO_ALL        (3UL << 6)   /* 内核/用户只读 */

/* [9:8] SH[1:0]：共享属性
 *   00 = Non-Shareable
 *   10 = Outer Shareable
 *   11 = Inner Shareable
 */
#define PD_SH_NONE          (0UL << 8)
#define PD_SH_OUTER         (2UL << 8)
#define PD_SH_INNER         (3UL << 8)   /* 内核通常使用 Inner Shareable */

/* [10] AF：Access Flag（访问标志）
 * 首次访问该页时由硬件置1（若AF=0则触发Access Flag Fault）。
 * 软件管理模式下手动置1以避免fault。
 */
#define PD_AF               (1UL << 10)

/* [11] nG：not Global
 * 0 = Global（所有ASID共享，适合内核）
 * 1 = not Global（ASID隔离，适合用户页）
 */
#define PD_NG               (1UL << 11)

/*
 * ============================================================
 * 高位属性位（Upper Page/Block Attributes）
 * ============================================================
 */

/* [51] DBM：Dirty Bit Modifier（硬件脏位，ARMv8.1，暂不使用）*/
/* [52] Contiguous：连续提示（TLB优化，暂不使用）*/

/* [53] PXN：Privileged eXecute Never
 * 1 = EL1/EL2 不可执行该内存区域（数据页保护）
 */
#define PD_PXN              (1UL << 53)

/* [54] UXN/XN：(Unprivileged) eXecute Never
 * 1 = EL0 不可执行（内核页应设此位防止用户态执行内核代码）
 */
#define PD_UXN              (1UL << 54)

/*
 * ============================================================
 * 常用属性组合（面向内核页表构建）
 * ============================================================
 */

/* 内核可执行代码页（.text）：内核RW，用户不可执行 */
#define PD_KERNEL_EXEC      (PD_AF | PD_SH_INNER | PD_AP_RW_EL1 | \
                             PD_UXN | PD_ATTRINDX(3))  /* MT_NORMAL */

/* 内核普通数据页（.data/.bss）：内核RW，无执行权限 */
#define PD_KERNEL_DATA      (PD_AF | PD_SH_INNER | PD_AP_RW_EL1 | \
                             PD_PXN | PD_UXN | PD_ATTRINDX(3))  /* MT_NORMAL */

/* MMIO设备页（Device memory，不可执行，不可缓存）*/
#define PD_DEVICE           (PD_AF | PD_SH_OUTER | PD_AP_RW_EL1 | \
                             PD_PXN | PD_UXN | PD_ATTRINDX(0))  /* MT_DEVICE_nGnRnE */

/*
 * ============================================================
 * 用户态属性组合（Phase 5 新增）
 * ============================================================
 */

/* 用户代码页（.text）：用户可执行+可读，内核不可执行 */
#define PD_USER_EXEC        (PD_AF | PD_SH_INNER | PD_AP_RO_ALL | \
                             PD_PXN | PD_NG | PD_ATTRINDX(3))  /* MT_NORMAL */

/* 用户数据页（.data/.bss/stack）：用户可读写，不可执行 */
#define PD_USER_DATA        (PD_AF | PD_SH_INNER | PD_AP_RW_ALL | \
                             PD_PXN | PD_UXN | PD_NG | PD_ATTRINDX(3))

/*
 * ============================================================
 * Block / Page descriptor 构建宏
 * ============================================================
 */

/* 1GB Block descriptor（L1/PUD 级别）
 * 物理地址必须 1GB 对齐（bits[29:0] = 0）*/
#define mk_block_desc(phys, attrs)  \
    (((unsigned long)(phys) & ~((1UL << PUD_SHIFT) - 1)) | (attrs) | PD_BLOCK)

/* 2MB Block descriptor（L2/PMD 级别）
 * 物理地址必须 2MB 对齐（bits[20:0] = 0）*/
#define mk_block_desc_2m(phys, attrs)  \
    (((unsigned long)(phys) & ~((1UL << PMD_SHIFT) - 1)) | (attrs) | PD_BLOCK)

/* 4KB Page descriptor（L3/PTE 级别）
 * 物理地址必须 4KB 对齐（bits[11:0] = 0）*/
#define mk_page_desc(phys, attrs)  \
    (((unsigned long)(phys) & PAGE_MASK) | (attrs) | PD_PAGE)

/*
 * ============================================================
 * Table descriptor 构建宏（指向下级页表）
 * ============================================================
 */
#define mk_table_desc(next_table_phys)  \
    (((unsigned long)(next_table_phys) & PAGE_MASK) | PD_TABLE)

/*
 * ============================================================
 * 页表项读取 / 判断辅助宏
 * ============================================================
 */
#define pte_valid(pte)      ((pte) & 1UL)
#define pte_is_table(pte)   (((pte) & 3UL) == PD_TABLE)
#define pte_is_block(pte)   (((pte) & 3UL) == PD_BLOCK)

/* 从Table descriptor中提取下级页表物理地址 */
#define table_phys(pte)     ((pte) & (~0UL << PAGE_SHIFT) & ((1UL << 48) - 1))

/* 从Block descriptor中提取1GB物理基址 */
#define block_phys_l1(pte)  ((pte) & (~0UL << PUD_SHIFT) & ((1UL << 48) - 1))

/* 从Block descriptor中提取2MB物理基址 */
#define block_phys_l2(pte)  ((pte) & (~0UL << PMD_SHIFT) & ((1UL << 48) - 1))

/*
 * ============================================================
 * 页表遍历索引宏（从虚拟地址提取各级索引）
 * ============================================================
 */
#define pgd_index(va)   (((unsigned long)(va) >> PGDIR_SHIFT) & (PTRS_PER_PGD - 1))
#define pud_index(va)   (((unsigned long)(va) >> PUD_SHIFT)   & (PTRS_PER_PUD - 1))
#define pmd_index(va)   (((unsigned long)(va) >> PMD_SHIFT)   & (PTRS_PER_PMD - 1))
#define pte_index(va)   (((unsigned long)(va) >> PAGE_SHIFT)  & (PTRS_PER_PTE - 1))

#endif /* __ASM_PGTABLE_H */
