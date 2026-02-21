/*
 * mm/page_alloc.c — buddy page allocator
 *
 * Linux reference: mm/page_alloc.c
 *   - alloc_pages() / __free_pages()
 *   - free_area[] per-order free lists
 *   - buddy merging in __free_one_page()
 *   - splitting in rmqueue() / expand()
 *
 * Buddy system invariant:
 *   A block of order N at address A has its "buddy" at A XOR (N pages).
 *   Two buddies of order N can merge into one block of order N+1 iff:
 *     - both are free
 *     - they have the same order
 *
 * Memory layout:
 *   mem_map[]  — flat array of struct page, one per physical page
 *                allocated by memblock before buddy init
 *   free_area[order].free_list — singly-linked list of free blocks
 *   free_area[order].nr_free   — count of free blocks
 *
 * Example: 128 MB / 4 KB = 32768 pages
 *   mem_map size: 32768 × 24 bytes = 768 KB
 */

#include "../include/types.h"
#include "../include/memory.h"
#include "../include/memblock.h"
#include "../include/page_alloc.h"
#include "../include/uart.h"

/* Global page descriptor array (indexed by PFN) */
struct page *mem_map;
ulong        mem_map_base_pfn;   /* PFN of mem_map[0] */
static ulong mem_map_nr_pages;   /* total pages tracked */

/* Per-order free list */
struct free_area {
    struct page *free_list;  /* head of singly-linked list */
    ulong        nr_free;    /* number of free blocks at this order */
};

static struct free_area free_area[MAX_ORDER];

/* ── helpers ──────────────────────────────────────────────────────────── */

ulong pfn_of(struct page *p)
{
    return (ulong)(p - mem_map) + mem_map_base_pfn;
}

struct page *page_of_pfn(ulong pfn)
{
    return &mem_map[pfn - mem_map_base_pfn];
}

/*
 * buddy_pfn — return the PFN of the buddy for block at @pfn, order @order
 * Buddy is found by flipping bit 'order' of the PFN.
 * See: __find_buddy_pfn() in mm/page_alloc.c
 */
static inline ulong buddy_pfn(ulong pfn, int order)
{
    return pfn ^ (1UL << order);
}

/* Push a page onto a free_area list */
static void list_push(struct free_area *fa, struct page *p)
{
    p->next      = fa->free_list;
    fa->free_list = p;
    fa->nr_free++;
    p->flags |= PG_buddy;
    p->order  = (int)(fa - free_area);
}

/* Remove a specific page from a free_area list */
static int list_remove(struct free_area *fa, struct page *target)
{
    struct page **pp = &fa->free_list;
    while (*pp) {
        if (*pp == target) {
            *pp = target->next;
            fa->nr_free--;
            target->flags &= ~PG_buddy;
            target->next   = NULL;
            return 1;
        }
        pp = &(*pp)->next;
    }
    return 0;   /* not found */
}

/* Pop the first page from a free_area list */
static struct page *list_pop(struct free_area *fa)
{
    struct page *p = fa->free_list;
    if (!p)
        return NULL;
    fa->free_list = p->next;
    fa->nr_free--;
    p->flags &= ~PG_buddy;
    p->next   = NULL;
    return p;
}

/* ── free path ────────────────────────────────────────────────────────── */

/*
 * __free_one_page — add a page back to the free lists, merging buddies
 *
 * Mirrors __free_one_page() in mm/page_alloc.c:
 *   1. Check if buddy is free and at the same order
 *   2. If so, remove buddy from its list, combine, try again at order+1
 *   3. Otherwise, insert at current order
 */
static void __free_one_page(struct page *page, ulong pfn, int order)
{
    while (order < MAX_ORDER - 1) {
        ulong  bpfn = buddy_pfn(pfn, order);

        /* Buddy must be within our managed range */
        if (bpfn < mem_map_base_pfn ||
            bpfn >= mem_map_base_pfn + mem_map_nr_pages)
            break;

        struct page *buddy = page_of_pfn(bpfn);

        /* Buddy must be free and at the same order */
        if (!(buddy->flags & PG_buddy) || buddy->order != order)
            break;

        /* Remove buddy from its free list */
        list_remove(&free_area[order], buddy);

        /* The merged block starts at the lower PFN */
        if (bpfn < pfn) {
            page = buddy;
            pfn  = bpfn;
        }
        order++;
    }

    list_push(&free_area[order], page);
}

/* ── alloc path ───────────────────────────────────────────────────────── */

/*
 * alloc_pages — allocate 2^order contiguous pages
 *
 * Mirrors rmqueue() → __rmqueue() in mm/page_alloc.c:
 *   1. Look for a free block at the requested order
 *   2. If none, look at higher orders and split
 *   3. Return the allocated block, put remainder back
 */
struct page *alloc_pages(int order)
{
    int current_order;

    for (current_order = order; current_order < MAX_ORDER; current_order++) {
        struct page *page = list_pop(&free_area[current_order]);
        if (!page)
            continue;

        /*
         * Split: if current_order > order, break the block into halves
         * and return the lower halves to the free lists.
         * See: expand() in mm/page_alloc.c
         */
        while (current_order > order) {
            current_order--;
            /* Upper half: same block address + 2^current_order pages */
            ulong  high_pfn  = pfn_of(page) + (1UL << current_order);
            struct page *high = page_of_pfn(high_pfn);
            list_push(&free_area[current_order], high);
        }

        page->order  = -1;   /* allocated — no longer on free list */
        page->flags &= ~PG_buddy;
        return page;
    }

    return NULL;   /* out of memory */
}

/* ── public API ───────────────────────────────────────────────────────── */

void free_pages(struct page *page, int order)
{
    ulong pfn = pfn_of(page);
    page->order = order;
    __free_one_page(page, pfn, order);
}

ulong alloc_page(void)
{
    struct page *p = alloc_pages(0);
    if (!p)
        return 0;
    return pfn_of(p) << PAGE_SHIFT;
}

void free_page(ulong phys)
{
    ulong pfn = phys >> PAGE_SHIFT;
    free_pages(page_of_pfn(pfn), 0);
}

/* ── init ─────────────────────────────────────────────────────────────── */

/*
 * page_alloc_init — set up mem_map and hand free pages to buddy
 *
 * Called after memblock_init() and mmu_init().
 * The pages used by kernel + page tables are reserved by memblock;
 * everything from memblock_free_start() to RAM end is free.
 */
void page_alloc_init(void)
{
    ulong free_start = memblock_free_start();
    ulong free_end   = PHYS_RAM_END;
    ulong base_pfn   = phys_to_pfn(PHYS_RAM_BASE);
    ulong nr_pages   = phys_to_pfn(PHYS_RAM_END) - base_pfn;
    ulong i;

    /* Allocate struct page array from memblock */
    mem_map          = (struct page *)memblock_alloc(
                           nr_pages * sizeof(struct page),
                           PAGE_SIZE);
    mem_map_base_pfn = base_pfn;
    mem_map_nr_pages = nr_pages;

    /* Mark all pages as reserved initially */
    for (i = 0; i < nr_pages; i++) {
        mem_map[i].flags = PG_reserved;
        mem_map[i].order = -1;
        mem_map[i].next  = NULL;
    }

    /*
     * Hand free pages to buddy.
     * We add them in MAX_ORDER-1 chunks where possible for efficiency.
     * See: memblock_free_pages() → __free_pages_core() in mm/page_alloc.c
     */
    ulong pfn = phys_to_pfn(ALIGN_UP(free_start, PAGE_SIZE));
    ulong end_pfn = phys_to_pfn(ALIGN_DOWN(free_end, PAGE_SIZE));

    while (pfn < end_pfn) {
        /* Find the largest order block that fits at this alignment */
        int order = MAX_ORDER - 1;
        while (order > 0) {
            ulong size = 1UL << order;
            if ((pfn & (size - 1)) == 0 && pfn + size <= end_pfn)
                break;
            order--;
        }
        struct page *p = page_of_pfn(pfn);
        p->flags = 0;   /* clear PG_reserved */
        free_pages(p, order);
        pfn += (1UL << order);
    }

    uart_puts("buddy: free ");
    uart_puthex64(free_start);
    uart_puts(" - ");
    uart_puthex64(free_end);
    uart_puts("  (");
    uart_putdec((free_end - free_start) / (1024 * 1024));
    uart_puts(" MB in buddy)\n");
}

void page_alloc_dump(void)
{
    int ord;
    ulong total_free = 0;

    uart_puts("buddy: free_area:\n");
    for (ord = 0; ord < MAX_ORDER; ord++) {
        if (free_area[ord].nr_free == 0)
            continue;
        uart_puts("  order ");
        uart_putdec(ord);
        uart_puts(": ");
        uart_putdec(free_area[ord].nr_free);
        uart_puts(" blocks × ");
        uart_putdec((1UL << ord) * 4);
        uart_puts(" KB = ");
        uart_putdec(free_area[ord].nr_free * (1UL << ord) * 4);
        uart_puts(" KB\n");
        total_free += free_area[ord].nr_free * (1UL << ord) * PAGE_SIZE;
    }
    uart_puts("buddy: total free = ");
    uart_putdec(total_free / (1024 * 1024));
    uart_puts(" MB\n");
}
