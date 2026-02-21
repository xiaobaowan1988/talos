#ifndef _MEMBLOCK_H
#define _MEMBLOCK_H

#include "types.h"

/*
 * memblock — early boot physical memory allocator
 * Linux reference: mm/memblock.c
 */

void  memblock_init(ulong start, ulong end);
void  memblock_reserve(ulong base, ulong size);
void *memblock_alloc(size_t size, size_t align);
void  memblock_dump(void);

ulong memblock_used(void);
ulong memblock_free_start(void);
ulong memblock_free_end(void);

#endif /* _MEMBLOCK_H */
