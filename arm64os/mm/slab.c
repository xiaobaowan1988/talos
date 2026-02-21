/*
 * mm/slab.c — simple slab allocator (kmalloc / kfree)
 *
 * Linux reference: mm/slub.c (the default allocator since 2.6.23)
 *   - kmem_cache_alloc() / kmem_cache_free()
 *   - kmalloc() is implemented as kmem_cache_alloc(kmalloc_caches[...])
 *
 * Design (simplified):
 *   - 10 fixed-size caches: 8, 16, 32, 64, 128, 256, 512, 1024, 2048, 4096
 *   - Each cache maintains a free list of objects
 *   - Free objects store a "next" pointer in their first 8 bytes
 *   - When a cache is empty, a new page is carved into objects (a "slab")
 *   - kfree() reads a header prepended to each allocation to find the cache
 *
 * Object layout in memory:
 *   [ struct alloc_hdr (8 bytes) | user data (size bytes) ]
 *   hdr->cache_idx tells kfree() which free list to return to
 */

#include "../include/types.h"
#include "../include/slab.h"
#include "../include/page_alloc.h"
#include "../include/uart.h"
#include "../include/memory.h"

/* Prepended to every kmalloc allocation */
struct alloc_hdr {
    u8 cache_idx;    /* index into kmalloc_caches[], 0xFF = large alloc */
    u8 _pad[7];
};

#define HDR_SIZE  sizeof(struct alloc_hdr)

/* Per-size slab cache descriptor */
struct kmem_cache {
    size_t       obj_size;   /* bytes per object (excluding header) */
    size_t       total_size; /* HDR_SIZE + obj_size */
    void        *free_list;  /* head of free object list */
    ulong        nr_free;    /* current free objects */
    ulong        nr_slabs;   /* pages allocated as slabs */
};

/* Cache sizes: 8 → 4096 bytes (powers of 2) */
#define NR_CACHES   10
static const size_t cache_sizes[NR_CACHES] = {
    8, 16, 32, 64, 128, 256, 512, 1024, 2048, 4096
};
static struct kmem_cache caches[NR_CACHES];

/* ── free list helpers ────────────────────────────────────────────────── */

/*
 * Free objects use their first 8 bytes as a "next" pointer.
 * This is exactly how Linux's SLUB stores the freelist pointer
 * (struct kmem_cache.offset = 0 by default).
 */
static inline void *obj_next(void *obj)
{
    return *(void **)obj;
}

static inline void obj_set_next(void *obj, void *next)
{
    *(void **)obj = next;
}

/* ── slab refill ──────────────────────────────────────────────────────── */

/*
 * cache_refill — allocate one page and carve it into objects
 * See: new_slab() in mm/slub.c
 */
static void cache_refill(struct kmem_cache *c)
{
    ulong page_phys = alloc_page();
    if (!page_phys) {
        uart_puts("slab: OOM refilling cache size=");
        uart_putdec(c->obj_size);
        uart_puts("\n");
        return;
    }

    c->nr_slabs++;

    /* Carve the page into total_size objects */
    u8  *p    = (u8 *)page_phys;
    u8  *end  = p + PAGE_SIZE;
    int  count = 0;

    while (p + c->total_size <= end) {
        obj_set_next(p, c->free_list);
        c->free_list = p;
        c->nr_free++;
        p += c->total_size;
        count++;
    }
}

/* ── public API ───────────────────────────────────────────────────────── */

void kmalloc_init(void)
{
    int i;
    for (i = 0; i < NR_CACHES; i++) {
        caches[i].obj_size   = cache_sizes[i];
        caches[i].total_size = HDR_SIZE + cache_sizes[i];
        caches[i].free_list  = NULL;
        caches[i].nr_free    = 0;
        caches[i].nr_slabs   = 0;
    }
    uart_puts("slab: kmalloc caches ready (8 – 4096 bytes)\n");
}

/*
 * kmalloc — allocate kernel memory
 * Mirrors kmalloc() → kmem_cache_alloc(kmalloc_caches[index], gfp)
 */
void *kmalloc(size_t size)
{
    int i;
    struct kmem_cache *c = NULL;

    if (size == 0)
        return NULL;

    /* Find the smallest cache that fits size + header */
    for (i = 0; i < NR_CACHES; i++) {
        if (size <= caches[i].obj_size) {
            c = &caches[i];
            break;
        }
    }

    if (!c) {
        /* Large allocation: use page allocator directly */
        int order = 0;
        ulong need = size + HDR_SIZE;
        while ((PAGE_SIZE << order) < need)
            order++;
        ulong phys = 0;
        struct page *p = alloc_pages(order);
        if (p) phys = pfn_of(p) << PAGE_SHIFT;
        if (!phys)
            return NULL;
        struct alloc_hdr *hdr = (struct alloc_hdr *)phys;
        hdr->cache_idx = 0xFF;
        hdr->_pad[0]   = (u8)order;
        return (void *)(phys + HDR_SIZE);
    }

    /* Refill if empty */
    if (!c->free_list)
        cache_refill(c);
    if (!c->free_list)
        return NULL;

    /* Pop from free list */
    void *obj       = c->free_list;
    c->free_list    = obj_next(obj);
    c->nr_free--;

    /* Write header */
    struct alloc_hdr *hdr = (struct alloc_hdr *)obj;
    hdr->cache_idx = (u8)(c - caches);

    return (void *)((u8 *)obj + HDR_SIZE);
}

/*
 * kfree — return memory to the appropriate cache
 * Mirrors kfree() → slab_free() in mm/slub.c
 */
void kfree(void *ptr)
{
    if (!ptr)
        return;

    struct alloc_hdr *hdr = (struct alloc_hdr *)((u8 *)ptr - HDR_SIZE);

    if (hdr->cache_idx == 0xFF) {
        /* Large alloc: return to page allocator */
        int order = (int)hdr->_pad[0];
        ulong phys = (ulong)hdr;
        ulong pfn  = phys >> PAGE_SHIFT;
        free_pages(&mem_map[pfn - (PHYS_RAM_BASE >> PAGE_SHIFT)], order);
        return;
    }

    struct kmem_cache *c = &caches[hdr->cache_idx];
    void *obj = (void *)hdr;
    obj_set_next(obj, c->free_list);
    c->free_list = obj;
    c->nr_free++;
}

void *kzalloc(size_t size)
{
    void *p = kmalloc(size);
    if (p) {
        u8 *b = (u8 *)p;
        size_t i;
        for (i = 0; i < size; i++)
            b[i] = 0;
    }
    return p;
}

void kmalloc_dump(void)
{
    int i;
    uart_puts("slab: cache sizes and usage:\n");
    for (i = 0; i < NR_CACHES; i++) {
        uart_puts("  ");
        uart_putdec(caches[i].obj_size);
        uart_puts("B: ");
        uart_putdec(caches[i].nr_slabs);
        uart_puts(" slabs, ");
        uart_putdec(caches[i].nr_free);
        uart_puts(" free\n");
    }
}
