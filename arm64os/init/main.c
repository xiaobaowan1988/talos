/*
 * init/main.c — kernel entry point (C)
 *
 * Called from boot/entry.S after:
 *   - Exception level dropped to EL1
 *   - Stack set up
 *   - BSS zeroed
 *   - Exception vectors installed
 *
 * Corresponds to: arch/arm64/kernel/setup.c start_kernel()
 *
 * Phase 1 goal: verify hardware, print system info, halt cleanly.
 */

#include "../include/uart.h"

/* ── ARM64 system register helpers ──────────────────────────────────────
 *
 * We read a handful of ID registers to print CPU info.
 * See: arch/arm64/kernel/cpuinfo.c, cpufeature.c
 */

static inline unsigned long read_midr(void)
{
    unsigned long v;
    __asm__ volatile("mrs %0, midr_el1" : "=r"(v));
    return v;
}

static inline unsigned long read_mpidr(void)
{
    unsigned long v;
    __asm__ volatile("mrs %0, mpidr_el1" : "=r"(v));
    return v;
}

static inline unsigned long read_current_el(void)
{
    unsigned long v;
    __asm__ volatile("mrs %0, CurrentEL" : "=r"(v));
    return (v >> 2) & 0x3;
}

static inline unsigned long read_sctlr_el1(void)
{
    unsigned long v;
    __asm__ volatile("mrs %0, sctlr_el1" : "=r"(v));
    return v;
}

static inline unsigned long read_id_aa64mmfr0(void)
{
    unsigned long v;
    __asm__ volatile("mrs %0, id_aa64mmfr0_el1" : "=r"(v));
    return v;
}

static inline unsigned long read_id_aa64pfr0(void)
{
    unsigned long v;
    __asm__ volatile("mrs %0, id_aa64pfr0_el1" : "=r"(v));
    return v;
}

/* ── MIDR decoding ───────────────────────────────────────────────────────
 *
 * MIDR_EL1 layout (ARMv8 ARM D7.2.66):
 *   [31:24] Implementer  (0x41 = ARM Ltd)
 *   [23:20] Variant
 *   [19:16] Architecture (0xF = ARMv8 ID scheme)
 *   [15: 4] PartNum      (0xD08 = Cortex-A72)
 *   [ 3: 0] Revision
 */
static void print_cpu_info(void)
{
    unsigned long midr  = read_midr();
    unsigned long mpidr = read_mpidr();

    unsigned int implementer = (midr >> 24) & 0xFF;
    unsigned int part        = (midr >>  4) & 0xFFF;
    unsigned int variant     = (midr >> 20) & 0xF;
    unsigned int revision    = (midr >>  0) & 0xF;

    uart_puts("CPU: MIDR_EL1 = ");
    uart_puthex64(midr);
    uart_puts("\n");

    uart_puts("     Implementer: ");
    uart_puthex64(implementer);
    if (implementer == 0x41)
        uart_puts(" (ARM Ltd)");
    uart_puts("\n");

    uart_puts("     PartNum: ");
    uart_puthex64(part);
    if (part == 0xD08)
        uart_puts(" (Cortex-A72)");
    else if (part == 0xD0C)
        uart_puts(" (Neoverse-N1)");
    uart_puts("\n");

    uart_puts("     Variant.Revision: r");
    uart_putdec(variant);
    uart_putc('p');
    uart_putdec(revision);
    uart_puts("\n");

    uart_puts("CPU: MPIDR_EL1 = ");
    uart_puthex64(mpidr);
    uart_puts("\n");

    /* Aff0 = CPU id within cluster */
    uart_puts("     CPU #");
    uart_putdec(mpidr & 0xFF);
    uart_puts(" (Aff0)\n");
}

/* ── Memory feature info ─────────────────────────────────────────────────
 *
 * ID_AA64MMFR0_EL1 encodes supported page sizes and PA range.
 * See: arch/arm64/kernel/cpufeature.c
 *   bits[3:0]  PARange  (0x5 → 48-bit PA, typical on virt machine)
 *   bits[31:28] TGran4  (0x0 → 4KB pages supported)
 */
static void print_mem_features(void)
{
    unsigned long mmfr0 = read_id_aa64mmfr0();
    unsigned int  pa_range = mmfr0 & 0xF;

    static const char * const pa_names[] = {
        "32-bit (4GB)",  "36-bit (64GB)",  "40-bit (1TB)",
        "42-bit (4TB)",  "44-bit (16TB)",  "48-bit (256TB)",
        "52-bit (4PB)",
    };

    uart_puts("MEM: ID_AA64MMFR0_EL1 = ");
    uart_puthex64(mmfr0);
    uart_puts("\n");

    uart_puts("     PA range: ");
    if (pa_range < 7)
        uart_puts(pa_names[pa_range]);
    else
        uart_puts("unknown");
    uart_puts("\n");

    uart_puts("     4KB pages: ");
    uart_puts(((mmfr0 >> 28) & 0xF) == 0 ? "supported\n" : "not supported\n");
}

/* ── Exception handler stubs ─────────────────────────────────────────────
 *
 * Called from entry.S vector stubs. In Phase 1 these just print
 * diagnostic info and halt — exactly how early Linux panics work.
 *
 * See: arch/arm64/kernel/traps.c do_serror(), do_mem_abort()
 *      arch/arm64/kernel/entry-common.c
 */

/* pt_regs layout must match SAVE_ALL in entry.S */
struct pt_regs {
    unsigned long regs[30];     /* x0 – x29 */
    unsigned long lr;           /* x30 */
    unsigned long _pad;
    unsigned long elr;          /* ELR_EL1 — faulting PC */
    unsigned long spsr;         /* SPSR_EL1 */
};

void exc_sync_el1_handler(struct pt_regs *regs,
                           unsigned long esr,
                           unsigned long far)
{
    uart_puts("\n*** SYNC EXCEPTION at EL1 ***\n");
    uart_puts("ESR_EL1 (syndrome): ");
    uart_puthex64(esr);
    uart_puts("\n");
    uart_puts("FAR_EL1 (fault addr): ");
    uart_puthex64(far);
    uart_puts("\n");
    uart_puts("ELR_EL1 (faulting PC): ");
    uart_puthex64(regs->elr);
    uart_puts("\n");
    /* Decode EC (Exception Class) from ESR bits[31:26] */
    uart_puts("EC: ");
    uart_puthex64((esr >> 26) & 0x3F);
    uart_puts("\n");
    uart_puts("HALT\n");
    while (1)
        __asm__ volatile("wfe");
}

void exc_irq_el1_handler(struct pt_regs *regs)
{
    /* Phase 1: no IRQ expected yet — will be replaced in Phase 3 */
    uart_puts("\n*** IRQ at EL1 (unexpected in Phase 1) ***\n");
    uart_puts("ELR: ");
    uart_puthex64(regs->elr);
    uart_puts("\n");
    while (1)
        __asm__ volatile("wfe");
}

void exc_sync_el0_handler(struct pt_regs *regs, unsigned long esr)
{
    /* Phase 1: no user space yet */
    uart_puts("\n*** SYNC at EL0 (unexpected in Phase 1) ***\n");
    while (1)
        __asm__ volatile("wfe");
}

/* ── Banner ──────────────────────────────────────────────────────────────*/
static void print_banner(void)
{
    uart_puts("\n");
    uart_puts("  ___  ____  __  __  ____  _  _    __  ____  ____  \n");
    uart_puts(" / __)(  _ \\(  \\/  )/ ___)( \\/ )  /  \\/ ___)/ ___) \n");
    uart_puts("( (_ \\ )   / )    ( \\___ \\ )  /  (  O \\___ \\\\___ \\ \n");
    uart_puts(" \\___/(__\\_)(_/\\/\\_)(____/(__/    \\__/(____/(____/ \n");
    uart_puts("\n");
    uart_puts("ARM64v8 OS  —  Phase 1: Boot\n");
    uart_puts("────────────────────────────\n\n");
}

/* ── kernel_main ─────────────────────────────────────────────────────────
 *
 * C entry point. Mirrors the role of start_kernel() in init/main.c
 * of the Linux kernel, but stripped to Phase 1 essentials.
 */
void kernel_main(void)
{
    /* First thing: bring up UART so we can print */
    uart_init();

    print_banner();

    /* Confirm exception level — should be 1 */
    uart_puts("Boot: Exception Level = EL");
    uart_putdec(read_current_el());
    uart_puts("\n");

    /* SCTLR_EL1: MMU and cache status */
    unsigned long sctlr = read_sctlr_el1();
    uart_puts("Boot: SCTLR_EL1 = ");
    uart_puthex64(sctlr);
    uart_puts("\n");
    uart_puts("      MMU ");
    uart_puts((sctlr & 1) ? "ON\n" : "OFF (expected)\n");
    uart_puts("      D-Cache ");
    uart_puts((sctlr & (1 << 2)) ? "ON\n" : "OFF (expected)\n");
    uart_puts("\n");

    print_cpu_info();
    uart_puts("\n");
    print_mem_features();

    uart_puts("\n");
    uart_puts("────────────────────────────\n");
    uart_puts("Phase 1 complete. System halted.\n");
    uart_puts("Next: Phase 2 — Memory Management (MMU + Buddy allocator)\n");

    /* Halt — wait for events (low-power spin) */
    while (1)
        __asm__ volatile("wfe");
}
