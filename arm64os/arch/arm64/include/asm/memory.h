/* SPDX-License-Identifier: GPL-2.0 */
/*
 * arm64os/arch/arm64/include/asm/memory.h
 *
 * 虚拟/物理地址空间布局常量
 *
 * 参考：arch/arm64/include/asm/memory.h
 *       ARMv8-A Architecture Reference Manual (ARM DDI 0487)
 *
 * Phase 2 实现：恒等映射（VA == PA），内核在低虚拟地址运行。
 * 高虚拟地址（KIMAGE_VADDR）留待后续阶段启用。
 */

#ifndef __ASM_MEMORY_H
#define __ASM_MEMORY_H

/*
 * ============================================================
 * 页大小与对齐
 * ============================================================
 */
#define PAGE_SHIFT          12
#define PAGE_SIZE           (1UL << PAGE_SHIFT)     /* 4KB */
#define PAGE_MASK           (~(PAGE_SIZE - 1))

/*
 * ============================================================
 * ARMv8-A 48位虚拟地址（4KB粒度）
 *
 * VA空间分为两半：
 *   TTBR0：低地址空间 [0x0000_0000_0000_0000, 0x0000_FFFF_FFFF_FFFF]
 *   TTBR1：高地址空间 [0xFFFF_0000_0000_0000, 0xFFFF_FFFF_FFFF_FFFF]
 *
 * TCR_EL1.T0SZ = TCR_EL1.T1SZ = 16（48位VA，256TB每个空间）
 * ============================================================
 */
#define VA_BITS             48
#define T0SZ_VALUE          (64 - VA_BITS)          /* = 16 */
#define T1SZ_VALUE          (64 - VA_BITS)          /* = 16 */

/*
 * ============================================================
 * 页表层级（4KB粒度，48位VA）
 *
 * L0 (PGD): VA[47:39]  — 9位，每项512GB
 * L1 (PUD): VA[38:30]  — 9位，每项1GB（可用1GB Block Descriptor）
 * L2 (PMD): VA[29:21]  — 9位，每项2MB（可用2MB Block Descriptor）
 * L3 (PTE): VA[20:12]  — 9位，每项4KB Page Descriptor
 * ============================================================
 */
#define PGDIR_SHIFT         39                      /* PGD: bits[47:39] */
#define PUD_SHIFT           30                      /* PUD: bits[38:30] */
#define PMD_SHIFT           21                      /* PMD: bits[29:21] */
#define PAGE_SHIFT_L3       12                      /* PTE: bits[20:12] */

#define PTRS_PER_PGD        512
#define PTRS_PER_PUD        512
#define PTRS_PER_PMD        512
#define PTRS_PER_PTE        512

#define PGD_SIZE            (1UL << PGDIR_SHIFT)    /* 512GB */
#define PUD_SIZE            (1UL << PUD_SHIFT)      /* 1GB */
#define PMD_SIZE            (1UL << PMD_SHIFT)      /* 2MB */

/*
 * ============================================================
 * 物理内存布局（QEMU virt machine）
 *
 * QEMU -M virt 将 1GB RAM 映射到 0x40000000 起始处。
 * 内核 ELF 被 QEMU 加载到 0x40080000（ARM64 Image 头偏移 512KB）。
 * ============================================================
 */
#define PHYS_OFFSET         0x40000000UL            /* 物理内存起始地址 */
#define PHYS_SIZE           0x40000000UL            /* 1GB RAM */
#define PHYS_END            (PHYS_OFFSET + PHYS_SIZE)   /* 0x80000000 */

/* 内核加载地址（QEMU virt 默认：PHYS_OFFSET + 512KB） */
#define KERNEL_PHYS_LOAD    0x40080000UL

/*
 * ============================================================
 * 高虚拟地址内核映射（Phase 2 预留，暂未启用）
 *
 * 在启用 TTBR1 高地址映射后，内核虚拟地址如下：
 *   KIMAGE_VADDR = 0xFFFF000000000000UL（内核镜像基址）
 *   PAGE_OFFSET  = 0xFFFF800000000000UL（物理内存线性映射基址）
 *
 * Phase 2 中内核继续在恒等映射（低VA == PA）中运行。
 * ============================================================
 */
#define KIMAGE_VADDR        0xFFFF000000000000UL
#define PAGE_OFFSET         0xFFFF800000000000UL

/*
 * ============================================================
 * 地址转换辅助宏（Phase 2 恒等映射下 VA == PA）
 * ============================================================
 */
#define phys_to_virt(phys)  ((void *)(unsigned long)(phys))
#define virt_to_phys(virt)  ((unsigned long)(virt))

/*
 * 将地址向下/上对齐到页边界
 */
#define PAGE_ALIGN_DOWN(x)  ((x) & PAGE_MASK)
#define PAGE_ALIGN_UP(x)    (((x) + PAGE_SIZE - 1) & PAGE_MASK)

/*
 * 物理帧号（PFN）与地址转换
 * PFN = (PA - PHYS_OFFSET) >> PAGE_SHIFT
 */
#define phys_to_pfn(phys)   (((phys) - PHYS_OFFSET) >> PAGE_SHIFT)
#define pfn_to_phys(pfn)    (((pfn) << PAGE_SHIFT) + PHYS_OFFSET)

#endif /* __ASM_MEMORY_H */
