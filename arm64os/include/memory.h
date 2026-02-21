#ifndef _MEMORY_H
#define _MEMORY_H

/*
 * Physical memory layout — QEMU 'virt' machine, -m 128M
 *
 *  0x00000000_09000000  PL011 UART
 *  0x00000000_0a000000  VirtIO MMIO bus
 *  0x00000000_40000000  RAM start  ← kernel loads here
 *  0x00000000_48000000  RAM end    (128 MB)
 *
 * See: arch/arm64/kernel/setup.c, arm64/mm/init.c
 */
#define PHYS_RAM_BASE   0x40000000UL
#define PHYS_RAM_SIZE   (128UL * 1024 * 1024)   /* 128 MB */
#define PHYS_RAM_END    (PHYS_RAM_BASE + PHYS_RAM_SIZE)

/* Page constants — mirrors arch/arm64/include/asm/page.h */
#define PAGE_SHIFT      12
#define PAGE_SIZE       (1UL << PAGE_SHIFT)     /* 4 KB */
#define PAGE_MASK       (~(PAGE_SIZE - 1))

/* 2 MB section (L1 block entry) */
#define SECTION_SHIFT   21
#define SECTION_SIZE    (1UL << SECTION_SHIFT)
#define SECTION_MASK    (~(SECTION_SIZE - 1))

/* Page frame number ↔ physical address */
#define phys_to_pfn(pa)  ((pa) >> PAGE_SHIFT)
#define pfn_to_phys(pfn) ((pfn) << PAGE_SHIFT)

/* Total pages in RAM */
#define NR_PAGES        (PHYS_RAM_SIZE / PAGE_SIZE)   /* 32768 */

#endif /* _MEMORY_H */
