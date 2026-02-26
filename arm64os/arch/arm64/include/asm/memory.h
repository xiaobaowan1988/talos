/* SPDX-License-Identifier: GPL-2.0 */
/*
 * arch/arm64/include/asm/memory.h
 *
 * Virtual address space layout for ARM64
 * Reference: arch/arm64/include/asm/memory.h
 */

#ifndef __ASM_MEMORY_H
#define __ASM_MEMORY_H

#define VA_BITS             48
#define PAGE_SHIFT          12
#define PAGE_SIZE           (1UL << PAGE_SHIFT)         /* 4KB */
#define PAGE_MASK           (~(PAGE_SIZE - 1))

/* Page table level shifts (4KB granule, 4-level) */
#define PGDIR_SHIFT         39      /* PGD (L0) index: bits [47:39] */
#define PUD_SHIFT           30      /* PUD (L1) index: bits [38:30] */
#define PMD_SHIFT           21      /* PMD (L2) index: bits [29:21] */
#define PTE_SHIFT           12      /* PTE (L3) index: bits [20:12] */

#define PTRS_PER_TABLE      512     /* 2^9 entries per level */
#define PTRS_PER_PGD        512
#define PTRS_PER_PUD        512
#define PTRS_PER_PMD        512
#define PTRS_PER_PTE        512

/*
 * Kernel virtual address space layout:
 *   KIMAGE_VADDR: kernel image mapping (TTBR1)
 *   PAGE_OFFSET:  linear map (physmem direct map)
 *
 * Phase 2: we run with identity mapping (VA == PA) since
 * we keep the kernel at physical addresses for simplicity.
 * The MMU is enabled with an identity map only.
 */
#define KIMAGE_VADDR        0xFFFF000000000000UL
#define PAGE_OFFSET         0xFFFF800000000000UL

/* QEMU virt machine physical memory starts at 0x40000000 (1GB) */
#define PHYS_OFFSET         0x40000000UL

/* Kernel is loaded at this physical address */
#define KERNEL_PHYS_BASE    0x40080000UL

/*
 * Phase 2 simplification: identity mapping only.
 * VA == PA, no high-address kernel mapping yet.
 */
#define __pa(x)     ((unsigned long)(x))
#define __va(x)     ((void *)(unsigned long)(x))

/* Physical memory size (QEMU -m 1G) */
#define PHYS_MEM_SIZE       (1UL << 30)     /* 1GB */
#define PHYS_MEM_END        (PHYS_OFFSET + PHYS_MEM_SIZE)

#endif /* __ASM_MEMORY_H */
