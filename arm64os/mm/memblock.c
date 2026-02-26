/* SPDX-License-Identifier: GPL-2.0 */
/*
 * mm/memblock.c
 *
 * Early physical memory allocator (bump allocator).
 *
 * Before the buddy allocator is ready, we need a simple way to
 * allocate memory for page tables, struct page array, etc.
 * memblock is a linear (bump) allocator that hands out memory
 * from the top of available RAM, never frees.
 *
 * Reference: mm/memblock.c
 */

#include <linux/types.h>
#include <asm/memory.h>
#include <asm/pgtable.h>

extern void boot_printk(const char *s);
extern void boot_printk_hex(unsigned long val);

/* Linker symbols */
extern char _end[];

/*
 * memblock state — simple bump allocator
 *
 * Allocates from [current, end) moving current upward.
 * All allocations are permanent (no free).
 */
static unsigned long memblock_current;
static unsigned long memblock_end;
static unsigned long memblock_total_allocated;

/*
 * memblock_init — initialize the early allocator
 *
 * Available memory starts after the kernel image (_end),
 * aligned to PAGE_SIZE.
 */
void memblock_init(void)
{
    memblock_current = PAGE_ALIGN((unsigned long)_end);
    memblock_end = PHYS_MEM_END;
    memblock_total_allocated = 0;

    boot_printk("[MEMBLOCK] Init: pool ");
    boot_printk_hex(memblock_current);
    boot_printk(" - ");
    boot_printk_hex(memblock_end);
    boot_printk("\n");
}

/*
 * memblock_alloc — allocate physically contiguous memory
 *
 * Returns a page-aligned physical address, or 0 on failure.
 * Memory is zeroed before return.
 */
unsigned long memblock_alloc(unsigned long size)
{
    unsigned long addr;
    unsigned long *p;
    unsigned long i;

    size = PAGE_ALIGN(size);
    addr = memblock_current;

    if (addr + size > memblock_end) {
        boot_printk("[MEMBLOCK] ERROR: out of memory!\n");
        return 0;
    }

    memblock_current += size;
    memblock_total_allocated += size;

    /* Zero the allocated memory */
    p = (unsigned long *)addr;
    for (i = 0; i < size / sizeof(unsigned long); i++)
        p[i] = 0;

    return addr;
}

/*
 * memblock_get_current — return current allocation pointer
 * (useful for buddy allocator to know where free memory starts)
 */
unsigned long memblock_get_current(void)
{
    return memblock_current;
}

/*
 * memblock_get_end — return end of physical memory
 */
unsigned long memblock_get_end(void)
{
    return memblock_end;
}

void memblock_dump_stats(void)
{
    boot_printk("[MEMBLOCK] Allocated: ");
    boot_printk_hex(memblock_total_allocated);
    boot_printk(" bytes (");
    boot_printk_hex(memblock_total_allocated >> PAGE_SHIFT);
    boot_printk(" pages)\n");
    boot_printk("[MEMBLOCK] Free pool: ");
    boot_printk_hex(memblock_current);
    boot_printk(" - ");
    boot_printk_hex(memblock_end);
    boot_printk("\n");
}
