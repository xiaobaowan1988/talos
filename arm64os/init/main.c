/*
 * init/main.c — kernel C entry point
 *
 * Phase 3: Exceptions & Interrupts
 *   Adds: irq_init → gic_init → timer_init → irq_enable → ticking
 *
 * Mirrors start_kernel() call chain:
 *   setup_arch()           memblock + MMU
 *   mm_core_init()         buddy + slab
 *   init_IRQ()             GIC init  (arch/arm64/kernel/irq.c)
 *   time_init()            arch timer (arch/arm64/kernel/time.c)
 *   local_irq_enable()     unmask PSTATE.I
 */

#include "../include/uart.h"
#include "../include/types.h"
#include "../include/memory.h"
#include "../include/memblock.h"
#include "../include/mmu.h"
#include "../include/page_alloc.h"
#include "../include/slab.h"
#include "../include/irq.h"
#include "../include/gic.h"
#include "../include/timer.h"

/* Linker symbols */
extern char _start[];
extern char _bss_end[];
extern char _stack_top[];

/* ── CPU info helpers ──────────────────────────────────────────────────── */
static inline ulong read_current_el(void)
{
    ulong v; __asm__ volatile("mrs %0, CurrentEL" : "=r"(v)); return (v>>2)&3;
}
static inline ulong read_midr(void)
{
    ulong v; __asm__ volatile("mrs %0, midr_el1" : "=r"(v)); return v;
}
static inline ulong read_sctlr_el1(void)
{
    ulong v; __asm__ volatile("mrs %0, sctlr_el1" : "=r"(v)); return v;
}

/* ── Phase 2 tests (kept for regression) ──────────────────────────────── */
static void test_buddy(void)
{
    ulong p1 = alloc_page();
    if (p1) {
        volatile u64 *ptr = (volatile u64 *)p1;
        *ptr = 0xDEADBEEFCAFEBABEUL;
        if (*ptr != 0xDEADBEEFCAFEBABEUL)
            uart_puts("  buddy: write FAIL\n");
        free_page(p1);
    }
}

static void test_kmalloc(void)
{
    char *s = (char *)kmalloc(32);
    if (s) { s[0]='O'; s[1]='K'; s[2]='\0'; kfree(s); }
}

/* ── kernel_main ───────────────────────────────────────────────────────── */
void kernel_main(void)
{
    uart_init();

    uart_puts("\nARM64v8 OS  --  Phase 3: Exceptions & Interrupts\n");
    uart_puts("-------------------------------------------------\n\n");

    uart_puts("Boot: EL");
    uart_putdec(read_current_el());
    uart_puts("  MIDR=");
    uart_puthex64(read_midr());
    uart_puts("\n\n");

    /* ── Phase 2: memory ──────────────────────────────────────────────── */
    memblock_init(PHYS_RAM_BASE, PHYS_RAM_END);
    memblock_reserve((ulong)_start, (ulong)_stack_top - (ulong)_start);

    mmu_init();

    ulong sctlr = read_sctlr_el1();
    uart_puts("MMU:  M="); uart_putdec(sctlr & 1);
    uart_puts(" C=");      uart_putdec((sctlr >> 2) & 1);
    uart_puts(" I=");      uart_putdec((sctlr >> 12) & 1);
    uart_puts("\n\n");

    page_alloc_init();
    kmalloc_init();

    test_buddy();
    test_kmalloc();
    uart_puts("MEM:  buddy + slab OK\n\n");

    /* ── Phase 3: interrupts ──────────────────────────────────────────── */

    /*
     * irq_init — zero the IRQ descriptor table
     * Mirrors init_IRQ() → irq_init_descs() in kernel/irq/irqdesc.c
     */
    irq_init();

    /*
     * gic_init — bring up GIC v3 (GICD + GICR + CPU interface)
     * Mirrors init_IRQ() → irqchip_init() → gic_of_init()
     *   in drivers/irqchip/irq-gic-v3.c
     */
    uart_puts("\n");
    gic_init();

    /*
     * timer_init — configure ARM arch timer, register INTID 30 handler
     * Mirrors time_init() → arch_timer_of_init()
     *   in drivers/clocksource/arm_arch_timer.c
     */
    uart_puts("\n");
    timer_init();

    /*
     * local_irq_enable() — clear PSTATE.I to unmask IRQs
     * Without this, no IRQ is ever delivered even if the GIC is configured.
     * See: arch/arm64/include/asm/irqflags.h arch_local_irq_enable()
     */
    uart_puts("\nIRQ:  enabling IRQs (clearing PSTATE.I)...\n");
    irq_enable();

    uart_puts("IRQ:  running — waiting for timer ticks\n");
    uart_puts("      (1 tick per 10 ms, heartbeat every 1 s)\n\n");

    /*
     * Idle loop — mirrors cpu_startup_entry() → do_idle() → cpu_idle_loop()
     * WFI = Wait For Interrupt: low-power state until an IRQ wakes the CPU.
     */
    ulong last = 0;
    while (1) {
        __asm__ volatile("wfi");

        /* After each tick, check if 5 seconds have passed */
        ulong t = timer_get_ticks();
        if (t >= 5 * (ulong)HZ && last < 5 * (ulong)HZ) {
            last = t;
            uart_puts("\n-------------------------------------------------\n");
            uart_puts("Phase 3 complete.\n");
            uart_puts("Next: Phase 4 -- Process Scheduling (CFS)\n");

            /* Disarm timer so output stops */
            __asm__ volatile("msr cntp_ctl_el0, %0\nisb" :: "r"(0UL));
            break;
        }
        last = t;
    }

    while (1) __asm__ volatile("wfe");
}
