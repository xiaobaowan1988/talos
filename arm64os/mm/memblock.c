/*
 * mm/memblock.c — early boot physical memory allocator
 *
 * Linux reference: mm/memblock.c
 * Used by: arch/arm64/mm/init.c arm64_memblock_init()
 *
 * Before the buddy allocator is ready we need to allocate memory for:
 *   - page tables (mmu_init)
 *   - struct page array (page_alloc_init)
 * This simple bump allocator serves that purpose.
 *
 * Design: a single contiguous free region [cursor, end).
 *   - Allocations advance the cursor upward (never freed individually).
 *   - memblock_reserve() marks ranges as used (advances cursor past them).
 *   - After page_alloc_init() the region [cursor, end) is handed to buddy.
 */

#include "../include/types.h"
#include "../include/memblock.h"
#include "../include/uart.h"
#include "../include/memory.h"

static ulong mb_start;   /* physical start of RAM */
static ulong mb_end;     /* physical end   of RAM */
static ulong mb_cursor;  /* next free physical byte */

/*
 * memblock_init — called once from kernel_main
 * start/end: physical addresses of the usable RAM range
 */
void memblock_init(ulong start, ulong end)
{
    mb_start  = start;
    mb_end    = end;
    mb_cursor = start;
}

/*
 * memblock_reserve — mark [base, base+size) as already used
 * Typically called to reserve the kernel image itself.
 * See: arm64_memblock_init() → memblock_reserve(__pa(_text), ...)
 */
void memblock_reserve(ulong base, ulong size)
{
    ulong res_end = ALIGN_UP(base + size, PAGE_SIZE);
    if (res_end > mb_cursor)
        mb_cursor = res_end;
}

/*
 * memblock_alloc — bump-allocate physically contiguous memory
 * Returns zeroed memory (mirrors memblock_alloc_try_nid zeroing).
 */
void *memblock_alloc(size_t size, size_t align)
{
    ulong addr, end;

    if (align == 0)
        align = 8;

    addr = ALIGN_UP(mb_cursor, align);
    end  = addr + size;

    if (end > mb_end) {
        uart_puts("memblock: OUT OF MEMORY\n");
        while (1) __asm__ volatile("wfe");
    }

    mb_cursor = end;

    /* Zero the allocated region (memblock zeroes by default in Linux) */
    u8 *p = (u8 *)addr;
    size_t i;
    for (i = 0; i < size; i++)
        p[i] = 0;

    return (void *)addr;
}

/* How many bytes have been consumed so far */
ulong memblock_used(void)
{
    return mb_cursor - mb_start;
}

/* How many bytes remain for the buddy allocator */
ulong memblock_free_start(void)
{
    return ALIGN_UP(mb_cursor, PAGE_SIZE);
}

ulong memblock_free_end(void)
{
    return mb_end;
}

void memblock_dump(void)
{
    uart_puts("memblock: RAM ");
    uart_puthex64(mb_start);
    uart_puts(" - ");
    uart_puthex64(mb_end);
    uart_puts("  (");
    uart_putdec((mb_end - mb_start) / (1024 * 1024));
    uart_puts(" MB)\n");

    uart_puts("memblock: used ");
    uart_puthex64(mb_start);
    uart_puts(" - ");
    uart_puthex64(mb_cursor);
    uart_puts("  (");
    uart_putdec((mb_cursor - mb_start) / 1024);
    uart_puts(" KB)\n");

    uart_puts("memblock: free ");
    uart_puthex64(memblock_free_start());
    uart_puts(" - ");
    uart_puthex64(mb_end);
    uart_puts("  (");
    uart_putdec((mb_end - memblock_free_start()) / (1024 * 1024));
    uart_puts(" MB)\n");
}
