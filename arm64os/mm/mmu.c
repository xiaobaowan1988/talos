/*
 * mm/mmu.c — ARM64 MMU initialisation
 *
 * Linux reference: arch/arm64/mm/mmu.c  (paging_init, map_kernel, etc.)
 *
 * Goal: build a minimal identity-map page table (VA = PA) and enable
 * the MMU so later phases can use virtual addresses and D-cache.
 *
 * Page table layout (4KB pages, T0SZ=25 → 39-bit VA, 3 levels):
 *
 *   TTBR0 (identity map, VA=PA):
 *     L0 table  [512 entries × 1GB each]  — 1 page = 4KB
 *     L1 table  [512 entries × 2MB each]  — 1 page per used L0 entry
 *
 *   We map two regions:
 *     1. RAM:  0x40000000 – 0x48000000  (128 MB, 64 × 2MB blocks)
 *              Normal WB cached, EL1 R/W, UXN
 *     2. UART: 0x09000000 (1 × 2MB block)
 *              Device nGnRnE, EL1 R/W, UXN+PXN
 *
 * After mmu_init() returns, we are still at the same PC (VA = PA),
 * MMU is ON, D-cache and I-cache are ON.
 *
 * Key registers set:
 *   MAIR_EL1  — memory attribute indirection
 *   TCR_EL1   — translation control (page size, address size, etc.)
 *   TTBR0_EL1 — base of L0 table
 *   SCTLR_EL1 — system control: M=1 (MMU), C=1 (Dcache), I=1 (Icache)
 */

#include "../include/types.h"
#include "../include/memory.h"
#include "../include/mmu.h"
#include "../include/memblock.h"
#include "../include/uart.h"

/* Page table entries are 8-byte (64-bit) descriptors */
typedef u64 pte_t;

/* A page table is an array of 512 entries */
#define PT_ENTRIES  512

static pte_t *l0_table;   /* one global L0 (PGD) */

/*
 * pt_alloc — allocate one zeroed page table from memblock
 */
static pte_t *pt_alloc(void)
{
    return (pte_t *)memblock_alloc(PT_ENTRIES * sizeof(pte_t), PAGE_SIZE);
}

/*
 * map_range_2mb — identity-map a physical range using 2MB block entries
 *
 * @phys_start: physical address (2MB aligned)
 * @phys_end:   physical end    (2MB aligned)
 * @flags:      PTE flags (BLOCK_KERNEL or BLOCK_DEVICE)
 *
 * ARM64 39-bit VA address decoding (4KB, 3-level):
 *   VA[38:30] = L0 index  (1GB per entry)
 *   VA[29:21] = L1 index  (2MB per entry) ← block descriptor
 *   VA[20:12] = L2 index  (not used here: we stop at L1 blocks)
 *   VA[11: 0] = page offset
 */
static void map_range_2mb(ulong phys_start, ulong phys_end, u64 flags)
{
    ulong addr;

    for (addr = phys_start; addr < phys_end; addr += SECTION_SIZE) {
        /* L0 index: VA[38:30] */
        u64 l0_idx = (addr >> 30) & 0x1FF;

        /* Get or create the L1 table for this L0 entry */
        if (!(l0_table[l0_idx] & PTE_VALID)) {
            pte_t *l1 = pt_alloc();
            l0_table[l0_idx] = (u64)l1 | PTE_VALID | PTE_TYPE_TABLE;
        }

        pte_t *l1_table = (pte_t *)(l0_table[l0_idx] & ~0xFFFUL);

        /* L1 index: VA[29:21] */
        u64 l1_idx = (addr >> 21) & 0x1FF;

        /* 2MB block descriptor: output_addr[47:21] | flags */
        l1_table[l1_idx] = (addr & ~(SECTION_SIZE - 1)) | flags;
    }
}

/*
 * mmu_init — build page tables and enable the MMU
 *
 * Sequence mirrors arch/arm64/mm/mmu.c paging_init():
 *   1. Allocate L0 table
 *   2. Map RAM (normal cached)
 *   3. Map device MMIO (device nGnRnE)
 *   4. Set MAIR_EL1, TCR_EL1, TTBR0_EL1
 *   5. TLB invalidate
 *   6. Set SCTLR_EL1.M (MMU on)
 */
void mmu_init(void)
{
    uart_puts("MMU:  building page tables...\n");

    /* Step 1: Allocate L0 (PGD) table */
    l0_table = pt_alloc();

    /* Step 2: Identity-map RAM (0x40000000 – 0x48000000) as normal WB */
    map_range_2mb(PHYS_RAM_BASE, PHYS_RAM_END, BLOCK_KERNEL);

    /* Step 3: Map UART device region as device memory */
    /* Round UART_BASE down to 2MB boundary: 0x09000000 → 0x08000000 */
    map_range_2mb(0x08000000UL, 0x0A000000UL, BLOCK_DEVICE);

    uart_puts("MMU:  L0 @ ");
    uart_puthex64((ulong)l0_table);
    uart_puts("\n");

    /*
     * Step 4a: MAIR_EL1 — memory attribute indirection register
     *   attr[0] = 0x00 (Device nGnRnE)
     *   attr[1] = 0x44 (Normal non-cacheable)
     *   attr[2] = 0xFF (Normal write-back, WA inner+outer)
     *
     * See: arch/arm64/mm/proc.S __cpu_setup, cpu_set_default_tcr_t1sz
     */
    __asm__ volatile("msr mair_el1, %0" :: "r"((u64)MAIR_VALUE));

    /*
     * Step 4b: TCR_EL1 — translation control register
     *   T0SZ=25 (39-bit VA), TG0=4KB, SH=inner, IRGN/ORGN=WB, IPS=48bit
     */
    __asm__ volatile("msr tcr_el1,  %0" :: "r"((u64)TCR_VALUE));

    /*
     * Step 4c: TTBR0_EL1 — L0 table base address
     *   Linux also sets ASID in bits[63:48]; we use ASID=0.
     */
    __asm__ volatile("msr ttbr0_el1, %0" :: "r"((u64)l0_table));

    /* Also point TTBR1 to same table (kernel space, unused for now) */
    __asm__ volatile("msr ttbr1_el1, %0" :: "r"((u64)l0_table));

    /* Instruction sync barrier — ensure register writes complete */
    __asm__ volatile("isb");

    /*
     * Step 5: Invalidate TLBs (all stages, all ASID)
     * See: arch/arm64/mm/tlb.h __flush_tlb_all
     */
    __asm__ volatile("tlbi vmalle1\n\t"
                     "dsb  sy\n\t"
                     "isb");

    /*
     * Step 6: Enable MMU, D-cache, I-cache in SCTLR_EL1
     *
     * CRITICAL: after this instruction VA=PA (identity map),
     * so the PC continues executing at the same address.
     * See: arch/arm64/mm/proc.S __enable_mmu
     */
    u64 sctlr;
    __asm__ volatile("mrs %0, sctlr_el1" : "=r"(sctlr));
    sctlr |= SCTLR_M | SCTLR_C | SCTLR_I;
    __asm__ volatile("msr sctlr_el1, %0\n\t"
                     "isb"
                     :: "r"(sctlr));

    uart_puts("MMU:  ON (identity map, D-cache ON, I-cache ON)\n");

    /* Verify: read SCTLR back */
    __asm__ volatile("mrs %0, sctlr_el1" : "=r"(sctlr));
    uart_puts("MMU:  SCTLR_EL1 = ");
    uart_puthex64(sctlr);
    uart_puts("\n");
}
