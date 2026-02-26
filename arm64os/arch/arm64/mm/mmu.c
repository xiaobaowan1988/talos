/* SPDX-License-Identifier: GPL-2.0 */
/*
 * arch/arm64/mm/mmu.c
 *
 * MMU initialization — build identity-mapped page tables and enable MMU.
 *
 * Phase 2 strategy:
 *   - Identity mapping only (VA == PA) for the first 1GB (0x40000000–0x7FFFFFFF)
 *   - Uses 1GB block descriptors at PGD/PUD level for simplicity
 *   - Device region (0x00000000–0x3FFFFFFF) mapped as Device-nGnRnE for UART etc.
 *   - After MMU is on, C code continues at the same physical addresses
 *
 * Reference: arch/arm64/mm/mmu.c, arch/arm64/mm/proc.S
 */

#include <linux/types.h>
#include <asm/memory.h>
#include <asm/pgtable.h>

/* Provided by printk.c */
extern void boot_printk(const char *s);
extern void boot_printk_hex(unsigned long val);

/* TLB flush helper (tlb.S) */
extern void tlb_flush_all(void);

/*
 * Page tables — statically allocated, zero-initialized (BSS).
 *
 * For identity mapping with 4KB granule and 48-bit VA:
 *   PGD (L0): 512 entries, each covers 512GB
 *   PUD (L1): 512 entries, each covers 1GB — we use 1GB block descriptors here
 *
 * We only need one PGD and one PUD table for the low 512GB identity map.
 */
static unsigned long pgd_table[PTRS_PER_TABLE] __attribute__((aligned(PAGE_SIZE)));
static unsigned long pud_table[PTRS_PER_TABLE] __attribute__((aligned(PAGE_SIZE)));

/*
 * create_page_tables — build identity-mapped page tables
 *
 * Maps the first 2GB of physical address space:
 *   [0x00000000 - 0x3FFFFFFF] → Device memory (UART, GIC, etc.)
 *   [0x40000000 - 0x7FFFFFFF] → Normal memory (RAM)
 *
 * Uses 1GB block descriptors at PUD (L1) level for simplicity.
 */
void create_page_tables(void)
{
    unsigned long i;

    boot_printk("[MMU] Creating identity-mapped page tables...\n");

    /* Clear tables */
    for (i = 0; i < PTRS_PER_TABLE; i++) {
        pgd_table[i] = 0;
        pud_table[i] = 0;
    }

    /*
     * PGD[0] → points to pud_table (covers VA 0x0000_0000_0000_0000 – 0x0000_007F_FFFF_FFFF)
     * This is a table descriptor: address of next-level table | PTE_TYPE_TABLE
     */
    pgd_table[0] = ((unsigned long)pud_table) | PTE_TYPE_TABLE;

    /*
     * PUD[0]: 1GB block at PA 0x00000000 — Device memory
     * Covers: UART (0x09000000), GIC (0x08000000), etc.
     */
    pud_table[0] = (0x00000000UL) | PTE_TYPE_BLOCK | PTE_DEVICE_FLAGS;

    /*
     * PUD[1]: 1GB block at PA 0x40000000 — Normal memory (RAM)
     * Covers: kernel image + physical RAM
     */
    pud_table[1] = (0x40000000UL) | PTE_TYPE_BLOCK | PTE_NORMAL_FLAGS;

    boot_printk("[MMU] PGD at      : ");
    boot_printk_hex((unsigned long)pgd_table);
    boot_printk("\n");
    boot_printk("[MMU] PUD at      : ");
    boot_printk_hex((unsigned long)pud_table);
    boot_printk("\n");
    boot_printk("[MMU] PUD[0]      : ");
    boot_printk_hex(pud_table[0]);
    boot_printk(" (Device 0-1GB)\n");
    boot_printk("[MMU] PUD[1]      : ");
    boot_printk_hex(pud_table[1]);
    boot_printk(" (Normal 1-2GB)\n");
}

/*
 * enable_mmu — configure and enable the MMU
 *
 * Steps:
 *   1. Set MAIR_EL1 (memory attribute indirection)
 *   2. Set TCR_EL1 (translation control)
 *   3. Set TTBR0_EL1 to pgd_table (identity map)
 *   4. Invalidate TLBs
 *   5. Enable MMU via SCTLR_EL1.M bit
 */
void enable_mmu(void)
{
    unsigned long sctlr;

    boot_printk("[MMU] Configuring MMU registers...\n");

    /* 1. MAIR_EL1: define memory attribute types */
    __asm__ volatile(
        "msr    mair_el1, %0\n"
        "isb\n"
        :
        : "r"(MAIR_EL1_VALUE)
    );

    /* 2. TCR_EL1: translation control register */
    __asm__ volatile(
        "msr    tcr_el1, %0\n"
        "isb\n"
        :
        : "r"(TCR_EL1_VALUE)
    );

    /* 3. TTBR0_EL1: translation table base register (identity map) */
    __asm__ volatile(
        "msr    ttbr0_el1, %0\n"
        "isb\n"
        :
        : "r"((unsigned long)pgd_table)
    );

    /* Clear TTBR1_EL1 (not used in Phase 2) */
    __asm__ volatile(
        "msr    ttbr1_el1, xzr\n"
        "isb\n"
    );

    boot_printk("[MMU] MAIR_EL1    : ");
    {
        unsigned long val;
        __asm__ volatile("mrs %0, mair_el1" : "=r"(val));
        boot_printk_hex(val);
    }
    boot_printk("\n");

    boot_printk("[MMU] TCR_EL1     : ");
    {
        unsigned long val;
        __asm__ volatile("mrs %0, tcr_el1" : "=r"(val));
        boot_printk_hex(val);
    }
    boot_printk("\n");

    boot_printk("[MMU] TTBR0_EL1   : ");
    {
        unsigned long val;
        __asm__ volatile("mrs %0, ttbr0_el1" : "=r"(val));
        boot_printk_hex(val);
    }
    boot_printk("\n");

    /* 4. Invalidate all TLB entries */
    tlb_flush_all();

    /* 5. Enable MMU: set SCTLR_EL1.M, also enable caches (C, I) */
    __asm__ volatile("mrs %0, sctlr_el1" : "=r"(sctlr));
    sctlr |= (1UL << 0);   /* M: MMU enable */
    sctlr |= (1UL << 2);   /* C: Data cache enable */
    sctlr |= (1UL << 12);  /* I: Instruction cache enable */
    sctlr |= (1UL << 3);   /* SA: Stack alignment check EL1 */
    sctlr |= (1UL << 4);   /* SA0: Stack alignment check EL0 */

    boot_printk("[MMU] Enabling MMU (SCTLR_EL1.M=1, C=1, I=1)...\n");

    __asm__ volatile(
        "dsb    sy\n"
        "msr    sctlr_el1, %0\n"
        "isb\n"
        :
        : "r"(sctlr)
    );

    boot_printk("[MMU] MMU enabled successfully!\n");

    /* Verify by reading SCTLR_EL1 back */
    __asm__ volatile("mrs %0, sctlr_el1" : "=r"(sctlr));
    boot_printk("[MMU] SCTLR_EL1   : ");
    boot_printk_hex(sctlr);
    boot_printk("\n");
}
