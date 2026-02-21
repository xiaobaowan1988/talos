#ifndef _MMU_H
#define _MMU_H

#include "types.h"

/*
 * MMU constants — ARM64 page table descriptors
 *
 * Reference: ARMv8-A Architecture Reference Manual
 *            arch/arm64/include/asm/pgtable-hwdef.h
 *            arch/arm64/mm/mmu.c
 *
 * We use 4KB pages, 3 levels (L0→L1→L2), 39-bit VA (T0SZ=25).
 *
 * Descriptor formats:
 *   Invalid:  bit[0] = 0
 *   Block:    bit[1:0] = 01  (L1=2MB, L2=4KB page → use 01 at L2)
 *   Table:    bit[1:0] = 11  (points to next-level table)
 */

/* Descriptor type bits */
#define PTE_VALID       (1UL << 0)
#define PTE_TYPE_TABLE  (1UL << 1)          /* 11 = table */
#define PTE_TYPE_BLOCK  (0UL << 1)          /* 01 = block  */
#define PTE_TYPE_PAGE   (1UL << 1)          /* 11 = page (at L2) */

/* Lower attribute bits [11:2] */
#define PTE_ATTRINDX(n) ((u64)(n) << 2)    /* MAIR index */
#define PTE_NS          (1UL << 5)
#define PTE_AP_RW_EL1   (0UL << 6)         /* EL1 R/W, EL0 none */
#define PTE_AP_RW_ALL   (1UL << 6)         /* EL1+EL0 R/W */
#define PTE_SH_NONE     (0UL << 8)
#define PTE_SH_OUTER    (2UL << 8)
#define PTE_SH_INNER    (3UL << 8)
#define PTE_AF          (1UL << 10)         /* Access Flag (must set!) */
#define PTE_NG          (1UL << 11)         /* not-global */

/* Upper attribute bits [63:51] */
#define PTE_PXN         (1UL << 53)         /* Privileged Execute Never */
#define PTE_UXN         (1UL << 54)         /* Unprivileged Execute Never */

/*
 * MAIR_EL1 memory attributes (index → byte in register)
 *   index 0: Device nGnRnE  (0x00) — UART, VirtIO
 *   index 1: Normal NC      (0x44) — not used yet
 *   index 2: Normal WB+WA   (0xFF) — RAM
 */
#define MAIR_DEVICE_nGnRnE  0x00UL
#define MAIR_NORMAL_NC      0x44UL
#define MAIR_NORMAL_WB      0xFFUL
#define MAIR_VALUE  (MAIR_DEVICE_nGnRnE       | \
                    (MAIR_NORMAL_NC   << 8)    | \
                    (MAIR_NORMAL_WB   << 16))

#define ATTR_DEVICE  0   /* MAIR index for device memory */
#define ATTR_NORMAL  2   /* MAIR index for normal (cached) memory */

/* Prebuilt descriptor flag sets */
#define BLOCK_KERNEL  (PTE_VALID | PTE_TYPE_BLOCK | PTE_AF | \
                       PTE_SH_INNER | PTE_AP_RW_EL1 | \
                       PTE_ATTRINDX(ATTR_NORMAL) | PTE_UXN)

#define BLOCK_DEVICE  (PTE_VALID | PTE_TYPE_BLOCK | PTE_AF | \
                       PTE_SH_OUTER | PTE_AP_RW_EL1 | \
                       PTE_ATTRINDX(ATTR_DEVICE) | PTE_UXN | PTE_PXN)

/*
 * TCR_EL1 — Translation Control Register
 *   T0SZ=25 → 39-bit VA (2^39 = 512GB per half)
 *   TG0=0   → 4KB granule (TTBR0)
 *   TG1=2   → 4KB granule (TTBR1)  ← encoding: 2 means 4KB for TG1
 *   SH/IRGN/ORGN = inner-shareable, write-back WA
 *   IPS=5   → 48-bit PA
 */
#define TCR_T0SZ(n)    ((u64)(n))
#define TCR_T1SZ(n)    ((u64)(n) << 16)
#define TCR_TG0_4K     (0UL << 14)
#define TCR_TG1_4K     (2UL << 30)
#define TCR_SH0_INNER  (3UL << 12)
#define TCR_SH1_INNER  (3UL << 28)
#define TCR_IRGN0_WB   (1UL << 8)
#define TCR_ORGN0_WB   (1UL << 10)
#define TCR_IRGN1_WB   (1UL << 24)
#define TCR_ORGN1_WB   (1UL << 26)
#define TCR_IPS_48BIT  (5UL << 32)
#define TCR_AS         (1UL << 36)   /* 16-bit ASID */

#define TCR_VALUE  (TCR_T0SZ(25)   | TCR_T1SZ(25)   | \
                    TCR_TG0_4K     | TCR_TG1_4K      | \
                    TCR_SH0_INNER  | TCR_SH1_INNER   | \
                    TCR_IRGN0_WB   | TCR_ORGN0_WB    | \
                    TCR_IRGN1_WB   | TCR_ORGN1_WB    | \
                    TCR_IPS_48BIT)

/* SCTLR_EL1 bits we care about */
#define SCTLR_M     (1UL << 0)   /* MMU enable */
#define SCTLR_C     (1UL << 2)   /* D-cache enable */
#define SCTLR_I     (1UL << 12)  /* I-cache enable */

void mmu_init(void);

#endif /* _MMU_H */
