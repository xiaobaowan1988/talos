/* SPDX-License-Identifier: GPL-2.0 */
/*
 * arch/arm64/include/asm/pgtable.h
 *
 * ARMv8-A page table entry definitions (4KB granule)
 * Reference: arch/arm64/include/asm/pgtable-hwdef.h
 *            ARMv8-A ARM, Section D5.3
 */

#ifndef __ASM_PGTABLE_H
#define __ASM_PGTABLE_H

#include <linux/types.h>
#include <asm/memory.h>

/*
 * Descriptor types (bits [1:0])
 *   0b00 = Invalid (fault)
 *   0b01 = Block descriptor (L1/L2 only) — maps 1GB/2MB
 *   0b11 = Table descriptor (L0/L1/L2) or Page descriptor (L3)
 */
#define PTE_TYPE_FAULT      0x0UL
#define PTE_TYPE_BLOCK      0x1UL       /* L1: 1GB block, L2: 2MB block */
#define PTE_TYPE_TABLE      0x3UL       /* Next-level table pointer */
#define PTE_TYPE_PAGE       0x3UL       /* L3: 4KB page */

#define PTE_VALID           (1UL << 0)  /* bit 0: valid descriptor */

/* Lower attributes (bits [11:2]) */
#define PTE_ATTRINDX(idx)   ((unsigned long)(idx) << 2)  /* Memory type index into MAIR */
#define PTE_NS              (1UL << 5)  /* Non-Secure */

/* AP[2:1] — Access Permission (bits [7:6]) */
#define PTE_AP_RW_EL1       (0UL << 6)  /* Kernel RW, User none */
#define PTE_AP_RW_ALL       (1UL << 6)  /* Kernel RW, User RW */
#define PTE_AP_RO_EL1       (2UL << 6)  /* Kernel RO, User none */
#define PTE_AP_RO_ALL       (3UL << 6)  /* Kernel RO, User RO */

/* SH[1:0] — Shareability (bits [9:8]) */
#define PTE_SH_NONE         (0UL << 8)
#define PTE_SH_OUTER        (2UL << 8)
#define PTE_SH_INNER        (3UL << 8)  /* Inner Shareable */

/* AF — Access Flag (bit 10) */
#define PTE_AF              (1UL << 10)

/* nG — not Global (bit 11): 1 = per-ASID TLB entry */
#define PTE_nG              (1UL << 11)

/* Upper attributes (bits [63:50]) */
#define PTE_PXN             (1UL << 53) /* Privileged Execute-Never */
#define PTE_UXN             (1UL << 54) /* Unprivileged Execute-Never */

/*
 * Memory attribute indices (MAIR_EL1)
 *
 * Phase 2: we use simple mapping where:
 *   index 0 = Device-nGnRnE (MMIO)
 *   index 1 = Normal Non-Cacheable
 *   index 2 = Normal (Write-Back, Read/Write-Allocate)
 */
#define MT_DEVICE_nGnRnE    0
#define MT_NORMAL_NC         1
#define MT_NORMAL            2

/* MAIR_EL1 value */
#define MAIR_EL1_VALUE  ( \
    (0x00UL << (8 * MT_DEVICE_nGnRnE)) | \
    (0x44UL << (8 * MT_NORMAL_NC))      | \
    (0xFFUL << (8 * MT_NORMAL))           \
)

/*
 * TCR_EL1 value for 4KB granule, 48-bit VA
 *
 * T0SZ = 16 (48-bit TTBR0 address space)
 * T1SZ = 16 (48-bit TTBR1 address space)
 * TG0  = 4KB
 * TG1  = 4KB
 * IRGN/ORGN = Write-Back Write-Allocate
 * SH   = Inner Shareable
 * IPS  = 40-bit (1TB physical address space, enough for QEMU)
 */
#define TCR_T0SZ_48         (16UL << 0)
#define TCR_T1SZ_48         (16UL << 16)
#define TCR_TG0_4K          (0UL << 14)
#define TCR_TG1_4K          (2UL << 30)
#define TCR_IRGN0_WBWA      (1UL << 8)
#define TCR_ORGN0_WBWA      (1UL << 10)
#define TCR_SH0_INNER       (3UL << 12)
#define TCR_IRGN1_WBWA      (1UL << 24)
#define TCR_ORGN1_WBWA      (1UL << 26)
#define TCR_SH1_INNER       (3UL << 28)
#define TCR_IPS_40BIT        (2UL << 32)

#define TCR_EL1_VALUE   ( \
    TCR_T0SZ_48     | TCR_T1SZ_48     | \
    TCR_TG0_4K      | TCR_TG1_4K      | \
    TCR_IRGN0_WBWA  | TCR_ORGN0_WBWA  | \
    TCR_SH0_INNER   |                    \
    TCR_IRGN1_WBWA  | TCR_ORGN1_WBWA  | \
    TCR_SH1_INNER   |                    \
    TCR_IPS_40BIT                        \
)

/*
 * Convenience macros for building page table entries
 */

/* Normal memory block/page: kernel RW, Inner Shareable, AF set */
#define PTE_NORMAL_FLAGS    ( \
    PTE_AF | PTE_SH_INNER | PTE_ATTRINDX(MT_NORMAL) | PTE_AP_RW_EL1 | PTE_UXN \
)

/* Device memory block/page: kernel RW, no cache, no execute */
#define PTE_DEVICE_FLAGS    ( \
    PTE_AF | PTE_ATTRINDX(MT_DEVICE_nGnRnE) | PTE_AP_RW_EL1 | PTE_UXN | PTE_PXN \
)

/* Kernel code (executable): normal memory, kernel RO, executable */
#define PTE_KERNEL_CODE_FLAGS ( \
    PTE_AF | PTE_SH_INNER | PTE_ATTRINDX(MT_NORMAL) | PTE_AP_RO_EL1 | PTE_UXN \
)

/* Helper: extract physical address from a descriptor (bits [47:12]) */
#define PTE_ADDR_MASK       0x0000FFFFFFFFF000UL

/* Number of pages needed for a given size */
#define PAGE_ALIGN(x)       (((x) + PAGE_SIZE - 1) & PAGE_MASK)
#define PFN_UP(x)           (((x) + PAGE_SIZE - 1) >> PAGE_SHIFT)
#define PFN_DOWN(x)         ((x) >> PAGE_SHIFT)

#endif /* __ASM_PGTABLE_H */
