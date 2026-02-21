#ifndef _SLAB_H
#define _SLAB_H

#include "types.h"

/*
 * Simple slab allocator — kmalloc / kfree
 *
 * Linux's slab (mm/slab.c) or slub (mm/slub.c) maintains per-size
 * caches backed by the buddy allocator.  Ours is a simplified version:
 *
 *   - Fixed-size free lists for common sizes (8, 16, 32, 64, 128,
 *     256, 512, 1024, 2048, 4096 bytes)
 *   - Each free object stores a "next" pointer in its first 8 bytes
 *   - Slabs are whole pages allocated from the buddy allocator
 *   - kfree() deduces the slab cache from the block header
 *
 * This is enough to understand the core kmalloc path without the full
 * complexity of per-CPU caches and memory pressure callbacks.
 */

void  kmalloc_init(void);
void *kmalloc(size_t size);
void  kfree(void *ptr);

/* Zeroing variant (mirrors kzalloc) */
void *kzalloc(size_t size);

void kmalloc_dump(void);

#endif /* _SLAB_H */
