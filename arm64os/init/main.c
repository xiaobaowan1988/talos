/*
 * init/main.c — kernel C entry point
 *
 * Phase 2: Memory Management
 *   Adds: memblock → MMU → buddy allocator → kmalloc/kfree
 *
 * Mirrors the call chain in init/main.c start_kernel():
 *   setup_arch()          → memblock_init, mmu_init
 *   mm_core_init()        → page_alloc_init, kmem_cache_init
 *   ... rest of kernel ...
 */

#include "../include/uart.h"
#include "../include/types.h"
#include "../include/memory.h"
#include "../include/memblock.h"
#include "../include/mmu.h"
#include "../include/page_alloc.h"
#include "../include/slab.h"

/* Linker symbols — defined in linker.ld */
extern char _start[];
extern char _bss_end[];
extern char _stack_top[];

/* ── CPU info helpers ──────────────────────────────────────────────────── */
static inline ulong read_current_el(void)
{
    ulong v;
    __asm__ volatile("mrs %0, CurrentEL" : "=r"(v));
    return (v >> 2) & 0x3;
}

static inline ulong read_midr(void)
{
    ulong v;
    __asm__ volatile("mrs %0, midr_el1" : "=r"(v));
    return v;
}

static inline ulong read_sctlr_el1(void)
{
    ulong v;
    __asm__ volatile("mrs %0, sctlr_el1" : "=r"(v));
    return v;
}

/* ── Phase 2 tests ─────────────────────────────────────────────────────── */

/*
 * test_buddy — exercise alloc_pages / free_pages
 */
static void test_buddy(void)
{
    uart_puts("\n[test] buddy allocator\n");

    ulong p1 = alloc_page();
    uart_puts("  alloc_page()    -> ");
    uart_puthex64(p1);
    if (p1) {
        volatile u64 *ptr = (volatile u64 *)p1;
        *ptr = 0xDEADBEEFCAFEBABEUL;
        uart_puts((*ptr == 0xDEADBEEFCAFEBABEUL) ? "  OK (writable)\n" : "  FAIL\n");
    } else {
        uart_puts("  FAIL (null)\n");
    }

    struct page *p2 = alloc_pages(3);   /* order-3 = 32 KB */
    uart_puts("  alloc_pages(3)  -> ");
    uart_puthex64(p2 ? pfn_of(p2) << PAGE_SHIFT : 0);
    uart_puts(p2 ? "  OK\n" : "  FAIL\n");

    if (p1) free_page(p1);
    if (p2) free_pages(p2, 3);

    uart_puts("  after free:\n");
    page_alloc_dump();

    ulong p3 = alloc_page();
    uart_puts("  re-alloc page   -> ");
    uart_puthex64(p3);
    uart_puts(p3 ? "  OK\n" : "  FAIL\n");
    if (p3) free_page(p3);
}

/*
 * test_kmalloc — exercise kmalloc / kfree
 */
static void test_kmalloc(void)
{
    uart_puts("\n[test] kmalloc / kfree\n");

    char *s = (char *)kmalloc(32);
    uart_puts("  kmalloc(32)     -> ");
    uart_puthex64((ulong)s);
    if (s) {
        const char *msg = "hello kernel!";
        int i = 0;
        while (msg[i]) { s[i] = msg[i]; i++; }
        s[i] = '\0';
        uart_puts("  \"");
        uart_puts(s);
        uart_puts("\"  OK\n");
        kfree(s);
    } else {
        uart_puts("  FAIL\n");
    }

    u64 *arr = (u64 *)kmalloc(512);
    uart_puts("  kmalloc(512)    -> ");
    uart_puthex64((ulong)arr);
    if (arr) {
        int i;
        for (i = 0; i < 64; i++) arr[i] = (u64)i * i;
        uart_puts("  arr[63]=");
        uart_putdec(arr[63]);
        uart_puts("  OK\n");
        kfree(arr);
    } else {
        uart_puts("  FAIL\n");
    }

    u8 *z = (u8 *)kzalloc(64);
    uart_puts("  kzalloc(64)     -> ");
    uart_puthex64((ulong)z);
    if (z) {
        int ok = 1, i;
        for (i = 0; i < 64; i++) if (z[i] != 0) { ok = 0; break; }
        uart_puts(ok ? "  zeroed  OK\n" : "  not zeroed  FAIL\n");
        kfree(z);
    }

    kmalloc_dump();
}

/* ── kernel_main ───────────────────────────────────────────────────────── */
void kernel_main(void)
{
    uart_init();

    uart_puts("\nARM64v8 OS  --  Phase 2: Memory Management\n");
    uart_puts("------------------------------------------\n\n");

    uart_puts("Boot: EL");
    uart_putdec(read_current_el());
    uart_puts("  MIDR=");
    uart_puthex64(read_midr());
    uart_puts("\n\n");

    /*
     * 1. memblock_init — register RAM, reserve kernel image
     *    See: arm64_memblock_init() in arch/arm64/mm/init.c
     */
    uart_puts("MEM:  memblock_init\n");
    memblock_init(PHYS_RAM_BASE, PHYS_RAM_END);
    memblock_reserve((ulong)_start, (ulong)_stack_top - (ulong)_start);
    memblock_dump();

    /*
     * 2. mmu_init — page tables + MMU enable
     *    See: paging_init() in arch/arm64/mm/mmu.c
     */
    uart_puts("\nMEM:  mmu_init\n");
    mmu_init();

    ulong sctlr = read_sctlr_el1();
    uart_puts("      SCTLR_EL1: MMU=");
    uart_putdec(sctlr & 1);
    uart_puts(" Dcache=");
    uart_putdec((sctlr >> 2) & 1);
    uart_puts(" Icache=");
    uart_putdec((sctlr >> 12) & 1);
    uart_puts("\n");

    /*
     * 3. page_alloc_init — hand free pages to buddy allocator
     *    See: free_area_init() in mm/page_alloc.c
     */
    uart_puts("\nMEM:  page_alloc_init\n");
    page_alloc_init();
    page_alloc_dump();

    /*
     * 4. kmalloc_init — set up per-size slab caches
     *    See: kmem_cache_init() in mm/slub.c
     */
    uart_puts("\nMEM:  kmalloc_init\n");
    kmalloc_init();

    /* 5. Tests */
    test_buddy();
    test_kmalloc();

    uart_puts("\n------------------------------------------\n");
    uart_puts("Phase 2 complete. System halted.\n");
    uart_puts("Next: Phase 3 -- Exceptions & GIC v3 Interrupts\n");

    while (1)
        __asm__ volatile("wfe");
}

/* ── exception handlers ────────────────────────────────────────────────── */

struct pt_regs {
    ulong regs[30];
    ulong lr, _pad, elr, spsr;
};

void exc_sync_el1_handler(struct pt_regs *regs, ulong esr, ulong far)
{
    uart_puts("\n*** SYNC EL1  ESR=");
    uart_puthex64(esr);
    uart_puts("  FAR=");
    uart_puthex64(far);
    uart_puts("  ELR=");
    uart_puthex64(regs->elr);
    uart_puts("\n");
    while (1) __asm__ volatile("wfe");
}

void exc_irq_el1_handler(struct pt_regs *regs)
{
    uart_puts("\n*** IRQ EL1  ELR=");
    uart_puthex64(regs->elr);
    uart_puts("\n");
    while (1) __asm__ volatile("wfe");
}

void exc_sync_el0_handler(struct pt_regs *regs, ulong esr)
{
    (void)regs; (void)esr;
    uart_puts("\n*** SYNC EL0 (unexpected)\n");
    while (1) __asm__ volatile("wfe");
}
