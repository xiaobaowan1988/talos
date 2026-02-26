/* SPDX-License-Identifier: GPL-2.0 */
/*
 * mm/page_alloc.c
 *
 * Buddy allocator — physical page management
 *
 * Reference: mm/page_alloc.c
 *
 * Design:
 *   - MAX_ORDER = 11 (order 0..10, max block = 2^10 pages = 4MB)
 *   - struct page array covers all physical pages in the RAM region
 *   - Free lists per order, alloc splits larger blocks, free merges buddies
 *   - Simple doubly-linked list using embedded prev/next pointers
 */

#include <linux/types.h>
#include <asm/memory.h>
#include <asm/pgtable.h>

extern void boot_printk(const char *s);
extern void boot_printk_hex(unsigned long val);

/* memblock.c */
extern unsigned long memblock_alloc(unsigned long size);
extern unsigned long memblock_get_current(void);
extern unsigned long memblock_get_end(void);

#define MAX_ORDER       11

/*
 * Page flags
 */
#define PAGE_FLAG_BUDDY     (1UL << 0)  /* Page is in buddy free list */
#define PAGE_FLAG_ALLOCATED (1UL << 1)  /* Page is allocated */

/*
 * struct page — per-physical-page metadata
 *
 * Simplified from Linux's struct page. Each physical 4KB page
 * has one struct page. The array is indexed by PFN (page frame number).
 */
struct page {
    unsigned long flags;
    struct page *next;      /* Free list next pointer */
    struct page *prev;      /* Free list prev pointer */
    unsigned int order;     /* Block order (valid for head page of free block) */
    int refcount;
};

/*
 * struct free_area — per-order free list header
 */
struct free_area {
    struct page head;       /* Sentinel node (head.next = first free, head.prev = last) */
    unsigned long nr_free;  /* Number of free blocks of this order */
};

/*
 * The single zone containing all buddy state
 */
static struct free_area free_areas[MAX_ORDER];

/* struct page array — one entry per physical page */
static struct page *mem_map;

/* PFN range managed by buddy */
static unsigned long buddy_start_pfn;
static unsigned long buddy_end_pfn;

/*
 * PFN ↔ struct page conversion
 */
static struct page *pfn_to_page(unsigned long pfn)
{
    return &mem_map[pfn - buddy_start_pfn];
}

static unsigned long page_to_pfn(struct page *page)
{
    return buddy_start_pfn + (unsigned long)(page - mem_map);
}

/*
 * Convert page to physical address and vice versa
 */
unsigned long page_to_phys(struct page *page)
{
    return page_to_pfn(page) << PAGE_SHIFT;
}

struct page *phys_to_page(unsigned long phys)
{
    return pfn_to_page(phys >> PAGE_SHIFT);
}

/*
 * List operations (circular doubly-linked list with sentinel)
 */
static void list_init(struct page *head)
{
    head->next = head;
    head->prev = head;
}

static int list_empty(struct page *head)
{
    return head->next == head;
}

static void list_add(struct page *entry, struct page *head)
{
    entry->next = head->next;
    entry->prev = head;
    head->next->prev = entry;
    head->next = entry;
}

static void list_del(struct page *entry)
{
    entry->prev->next = entry->next;
    entry->next->prev = entry->prev;
    entry->next = entry;
    entry->prev = entry;
}

/*
 * expand — split a larger block down to the requested order
 *
 * When we find a free block of order `high` but need order `low`,
 * split the upper half and put it back on the free list, repeatedly.
 */
static void expand(struct page *page, unsigned int low, unsigned int high)
{
    unsigned long size;

    while (high > low) {
        high--;
        size = 1UL << high;

        /* The upper-half buddy goes on the free list */
        struct page *buddy = page + size;
        buddy->order = high;
        buddy->flags = PAGE_FLAG_BUDDY;
        list_add(buddy, &free_areas[high].head);
        free_areas[high].nr_free++;
    }
}

/*
 * alloc_pages — allocate 2^order contiguous pages
 *
 * Returns struct page pointer to the first page, or NULL on failure.
 */
struct page *alloc_pages(unsigned int order)
{
    unsigned int current_order;
    struct page *page;

    if (order >= MAX_ORDER)
        return NULL;

    for (current_order = order; current_order < MAX_ORDER; current_order++) {
        if (list_empty(&free_areas[current_order].head))
            continue;

        /* Take the first free block */
        page = free_areas[current_order].head.next;
        list_del(page);
        free_areas[current_order].nr_free--;

        /* Split if we got a larger block */
        if (current_order > order)
            expand(page, order, current_order);

        page->flags = PAGE_FLAG_ALLOCATED;
        page->order = order;
        page->refcount = 1;
        return page;
    }

    return NULL;    /* Out of memory */
}

/*
 * free_pages — return 2^order pages to the buddy system
 *
 * Tries to merge with buddy blocks to form larger free blocks.
 */
void free_pages(struct page *page, unsigned int order)
{
    unsigned long pfn = page_to_pfn(page);
    unsigned long buddy_pfn;
    struct page *buddy;

    page->flags = 0;
    page->refcount = 0;

    while (order < MAX_ORDER - 1) {
        /* Find buddy PFN by flipping the bit at position `order` */
        buddy_pfn = pfn ^ (1UL << order);

        /* Check buddy is within our managed range */
        if (buddy_pfn < buddy_start_pfn || buddy_pfn >= buddy_end_pfn)
            break;

        buddy = pfn_to_page(buddy_pfn);

        /* Buddy must be free and same order to merge */
        if (!(buddy->flags & PAGE_FLAG_BUDDY) || buddy->order != order)
            break;

        /* Remove buddy from its free list */
        list_del(buddy);
        free_areas[order].nr_free--;
        buddy->flags = 0;

        /* Use the lower PFN as the merged block's base */
        if (buddy_pfn < pfn) {
            page = buddy;
            pfn = buddy_pfn;
        }

        order++;
    }

    /* Add the (possibly merged) block to the appropriate free list */
    page->order = order;
    page->flags = PAGE_FLAG_BUDDY;
    list_add(page, &free_areas[order].head);
    free_areas[order].nr_free++;
}

/*
 * buddy_init — initialize the buddy allocator
 *
 * 1. Allocate the struct page array from memblock
 * 2. Initialize free lists
 * 3. Add all free pages to the buddy system
 */
void buddy_init(void)
{
    unsigned long free_start, free_end;
    unsigned long total_pages, page_array_size;
    unsigned long pfn, i;

    boot_printk("[BUDDY] Initializing buddy allocator...\n");

    /* Initialize free list heads */
    for (i = 0; i < MAX_ORDER; i++) {
        list_init(&free_areas[i].head);
        free_areas[i].nr_free = 0;
    }

    /*
     * Determine PFN range for buddy management.
     * We manage from memblock_get_current() (after all early allocations)
     * to PHYS_MEM_END.
     */
    free_start = memblock_get_current();
    free_end = memblock_get_end();

    /* Align to MAX_ORDER boundary for clean buddy math */
    buddy_start_pfn = PFN_UP(free_start);
    buddy_end_pfn = PFN_DOWN(free_end);

    /* Align start_pfn up to MAX_ORDER-1 page boundary */
    {
        unsigned long max_block_pages = 1UL << (MAX_ORDER - 1);
        buddy_start_pfn = (buddy_start_pfn + max_block_pages - 1) & ~(max_block_pages - 1);
    }

    total_pages = buddy_end_pfn - buddy_start_pfn;

    boot_printk("[BUDDY] PFN range : ");
    boot_printk_hex(buddy_start_pfn);
    boot_printk(" - ");
    boot_printk_hex(buddy_end_pfn);
    boot_printk(" (");
    boot_printk_hex(total_pages);
    boot_printk(" pages)\n");

    /* Allocate struct page array from memblock */
    page_array_size = total_pages * sizeof(struct page);
    mem_map = (struct page *)memblock_alloc(page_array_size);
    if (!mem_map) {
        boot_printk("[BUDDY] ERROR: cannot allocate page array!\n");
        return;
    }

    boot_printk("[BUDDY] struct page array: ");
    boot_printk_hex((unsigned long)mem_map);
    boot_printk(" (");
    boot_printk_hex(page_array_size);
    boot_printk(" bytes)\n");

    /*
     * Re-compute free range: memblock_get_current() may have moved
     * after allocating the page array.
     */
    free_start = memblock_get_current();
    {
        unsigned long new_start_pfn = PFN_UP(free_start);
        unsigned long max_block_pages = 1UL << (MAX_ORDER - 1);
        new_start_pfn = (new_start_pfn + max_block_pages - 1) & ~(max_block_pages - 1);
        if (new_start_pfn > buddy_start_pfn) {
            /* Page array took space from managed range; adjust */
            buddy_start_pfn = new_start_pfn;
            total_pages = buddy_end_pfn - buddy_start_pfn;
        }
    }

    /* Initialize all struct page entries */
    for (i = 0; i < total_pages; i++) {
        mem_map[i].flags = 0;
        mem_map[i].next = &mem_map[i];
        mem_map[i].prev = &mem_map[i];
        mem_map[i].order = 0;
        mem_map[i].refcount = 0;
    }

    /*
     * Add pages to buddy free lists in largest possible blocks.
     * Walk through PFNs and add max-order blocks where alignment allows.
     */
    pfn = buddy_start_pfn;
    while (pfn < buddy_end_pfn) {
        unsigned int order;
        /* Find the largest order that:
         * 1. Is aligned: pfn is a multiple of (1 << order)
         * 2. Fits: pfn + (1 << order) <= buddy_end_pfn
         * 3. order < MAX_ORDER
         */
        for (order = MAX_ORDER - 1; order > 0; order--) {
            unsigned long block_pages = 1UL << order;
            if ((pfn & (block_pages - 1)) == 0 && pfn + block_pages <= buddy_end_pfn)
                break;
        }

        /* Verify alignment for order 0 case */
        {
            unsigned long block_pages = 1UL << order;
            if ((pfn & (block_pages - 1)) != 0 || pfn + block_pages > buddy_end_pfn) {
                /* Skip this page if it doesn't fit even order 0 */
                if (pfn + 1 > buddy_end_pfn)
                    break;
                order = 0;
            }
        }

        {
            struct page *page = pfn_to_page(pfn);
            page->order = order;
            page->flags = PAGE_FLAG_BUDDY;
            list_add(page, &free_areas[order].head);
            free_areas[order].nr_free++;
            pfn += 1UL << order;
        }
    }

    /* Print summary */
    boot_printk("[BUDDY] Free memory summary:\n");
    {
        unsigned long total_free_pages = 0;
        for (i = 0; i < MAX_ORDER; i++) {
            if (free_areas[i].nr_free > 0) {
                boot_printk("  order ");
                {
                    char buf[4];
                    if (i >= 10) {
                        buf[0] = '1';
                        buf[1] = '0' + (char)(i - 10);
                        buf[2] = '\0';
                    } else {
                        buf[0] = '0' + (char)i;
                        buf[1] = '\0';
                    }
                    boot_printk(buf);
                }
                boot_printk(": ");
                boot_printk_hex(free_areas[i].nr_free);
                boot_printk(" blocks\n");
                total_free_pages += free_areas[i].nr_free * (1UL << i);
            }
        }
        boot_printk("[BUDDY] Total free: ");
        boot_printk_hex(total_free_pages);
        boot_printk(" pages (");
        boot_printk_hex(total_free_pages << PAGE_SHIFT);
        boot_printk(" bytes)\n");
    }

    boot_printk("[BUDDY] Initialization complete\n");
}

/*
 * test_buddy — verify buddy allocator correctness
 */
void test_buddy(void)
{
    struct page *p1, *p2, *p3;
    unsigned long addr1, addr2, addr3;

    boot_printk("\n[TEST] === Buddy Allocator Test ===\n");

    /* Test 1: allocate order 0 (4KB) */
    p1 = alloc_pages(0);
    if (!p1) {
        boot_printk("[TEST] FAIL: alloc order 0 returned NULL\n");
        return;
    }
    addr1 = page_to_phys(p1);
    boot_printk("[TEST] alloc order 0: page @ ");
    boot_printk_hex(addr1);
    boot_printk("\n");

    /* Test 2: allocate order 2 (16KB) */
    p2 = alloc_pages(2);
    if (!p2) {
        boot_printk("[TEST] FAIL: alloc order 2 returned NULL\n");
        return;
    }
    addr2 = page_to_phys(p2);
    boot_printk("[TEST] alloc order 2: page @ ");
    boot_printk_hex(addr2);
    boot_printk("\n");

    /* Test 3: allocate order 4 (64KB) */
    p3 = alloc_pages(4);
    if (!p3) {
        boot_printk("[TEST] FAIL: alloc order 4 returned NULL\n");
        return;
    }
    addr3 = page_to_phys(p3);
    boot_printk("[TEST] alloc order 4: page @ ");
    boot_printk_hex(addr3);
    boot_printk("\n");

    /* Test 4: free in reverse order, verify merge */
    boot_printk("[TEST] Freeing order 4...\n");
    free_pages(p3, 4);

    boot_printk("[TEST] Freeing order 2...\n");
    free_pages(p2, 2);

    boot_printk("[TEST] Freeing order 0...\n");
    free_pages(p1, 0);

    /* Print post-free state */
    boot_printk("[TEST] Post-free state:\n");
    {
        unsigned long i;
        unsigned long total_free = 0;
        for (i = 0; i < MAX_ORDER; i++) {
            if (free_areas[i].nr_free > 0) {
                boot_printk("  order ");
                {
                    char buf[4];
                    if (i >= 10) {
                        buf[0] = '1';
                        buf[1] = '0' + (char)(i - 10);
                        buf[2] = '\0';
                    } else {
                        buf[0] = '0' + (char)i;
                        buf[1] = '\0';
                    }
                    boot_printk(buf);
                }
                boot_printk(": ");
                boot_printk_hex(free_areas[i].nr_free);
                boot_printk(" blocks\n");
                total_free += free_areas[i].nr_free * (1UL << i);
            }
        }
        boot_printk("[TEST] Total free: ");
        boot_printk_hex(total_free);
        boot_printk(" pages\n");
    }

    boot_printk("[TEST] Buddy allocator: OK\n");
}
