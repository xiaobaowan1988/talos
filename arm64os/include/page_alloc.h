#ifndef _PAGE_ALLOC_H
#define _PAGE_ALLOC_H

#include "types.h"

#define MAX_ORDER   11   /* orders 0..10 */

/*
 * struct page — one descriptor per physical page frame
 * Linux reference: include/linux/mm_types.h
 */
struct page {
    unsigned long flags;
    int           order;
    struct page  *next;
};

#define PG_buddy    (1UL << 0)
#define PG_reserved (1UL << 1)

extern struct page *mem_map;
extern ulong        mem_map_base_pfn;

void         page_alloc_init(void);
struct page *alloc_pages(int order);
void         free_pages(struct page *page, int order);
ulong        alloc_page(void);
void         free_page(ulong phys);
void         page_alloc_dump(void);

/* PFN ↔ struct page — non-static so they link across .c files */
ulong        pfn_of(struct page *p);
struct page *page_of_pfn(ulong pfn);

static inline ulong page_to_phys(struct page *p)
{
    return pfn_of(p) << 12;
}

#endif /* _PAGE_ALLOC_H */
