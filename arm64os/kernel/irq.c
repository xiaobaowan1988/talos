/*
 * kernel/irq.c — IRQ dispatch framework + EL1 exception handlers
 *
 * Linux reference:
 *   kernel/irq/manage.c   request_irq(), free_irq()
 *   kernel/irq/handle.c   handle_irq_event(), irq_dispatch()
 *   arch/arm64/kernel/irq.c  arm64_do_IRQ()
 *   arch/arm64/kernel/entry-common.c  el1_interrupt()
 *   arch/arm64/kernel/traps.c  do_serror(), do_mem_abort()
 *
 * Flow when an IRQ fires:
 *   CPU → vectors[EL1_IRQ] (entry.S)
 *   → SAVE_ALL
 *   → exc_irq_el1_handler(pt_regs *)    ← this file
 *   → gic_ack()  → INTID
 *   → irq_dispatch(INTID)
 *   → irq_table[INTID].handler()
 *   → gic_eoi(INTID)
 *   ← RESTORE_ALL → eret
 */

#include "../include/irq.h"
#include "../include/gic.h"
#include "../include/uart.h"
#include "../include/types.h"

/* ── IRQ descriptor table ─────────────────────────────────────────────── */

struct irq_desc irq_table[MAX_IRQS];

void irq_init(void)
{
    int i;
    for (i = 0; i < MAX_IRQS; i++) {
        irq_table[i].handler = NULL;
        irq_table[i].data    = NULL;
        irq_table[i].name    = NULL;
        irq_table[i].count   = 0;
    }
    uart_puts("IRQ:  table initialised (");
    uart_putdec(MAX_IRQS);
    uart_puts(" slots)\n");
}

/*
 * request_irq — register an interrupt handler
 *
 * Mirrors request_irq() → __setup_irq() in kernel/irq/manage.c.
 * In Linux this also enables the IRQ via the irq_chip; here we leave
 * gic_enable_irq() to the caller (timer_init does it explicitly).
 */
int request_irq(int irq, irq_handler_t handler, void *data, const char *name)
{
    if (irq < 0 || irq >= MAX_IRQS)
        return -1;
    irq_table[irq].handler = handler;
    irq_table[irq].data    = data;
    irq_table[irq].name    = name;
    irq_table[irq].count   = 0;
    uart_puts("IRQ:  registered IRQ ");
    uart_putdec(irq);
    uart_puts(" -> ");
    uart_puts(name ? name : "(unnamed)");
    uart_puts("\n");
    return 0;
}

/*
 * irq_dispatch — call the handler registered for @irq
 *
 * Mirrors handle_irq_event() in kernel/irq/handle.c.
 */
void irq_dispatch(int irq)
{
    if (irq < 0 || irq >= MAX_IRQS)
        return;

    struct irq_desc *desc = &irq_table[irq];
    desc->count++;

    if (desc->handler) {
        desc->handler(irq, desc->data);
    } else {
        uart_puts("IRQ:  unhandled INTID ");
        uart_putdec(irq);
        uart_puts("\n");
    }
}

/* ── EL1 exception handlers ───────────────────────────────────────────── */

/*
 * exc_irq_el1_handler — EL1 IRQ entry point
 *
 * Called from entry.S _exc_irq_el1 with SAVE_ALL frame on stack.
 * Mirrors arch/arm64/kernel/entry-common.c el1_interrupt()
 *         → arm64_do_IRQ() in arch/arm64/kernel/irq.c
 */
void exc_irq_el1_handler(struct pt_regs *regs)
{
    (void)regs;

    /*
     * 1. Acknowledge: read ICC_IAR1_EL1 → INTID
     *    This also deactivates priority-drop on the CPU interface.
     */
    int intid = gic_ack();

    /* INTID 1023 = spurious interrupt (no real IRQ pending) */
    if (intid == INTID_SPURIOUS)
        return;

    /* 2. Dispatch to registered handler */
    irq_dispatch(intid);

    /*
     * 3. End-of-Interrupt: write ICC_EOIR1_EL1
     *    Signals the CPU interface that handling is complete.
     *    See: gic_irq_eoi() in drivers/irqchip/irq-gic-v3.c
     */
    gic_eoi(intid);
}

/*
 * exc_sync_el1_handler — EL1 synchronous exception (data/instruction abort)
 *
 * Mirrors arch/arm64/kernel/traps.c do_mem_abort(), do_undefinstr()
 * ESR_EL1[31:26] = EC (Exception Class):
 *   0x21 = Instruction Abort (current EL)
 *   0x25 = Data Abort (current EL)
 *   0x0F = SVC from AArch64 (shouldn't happen at EL1)
 */
void exc_sync_el1_handler(struct pt_regs *regs, ulong esr, ulong far)
{
    u32 ec  = (esr >> 26) & 0x3F;
    u32 iss = esr & 0xFFFFFF;

    uart_puts("\n*** KERNEL SYNC EXCEPTION ***\n");
    uart_puts("ESR_EL1 = "); uart_puthex64(esr);
    uart_puts("  EC=");      uart_puthex64(ec);

    /* Decode common EC values */
    switch (ec) {
    case 0x21: uart_puts(" (Instr Abort, EL1)"); break;
    case 0x25: uart_puts(" (Data Abort,  EL1)"); break;
    case 0x20: uart_puts(" (Instr Abort, EL0)"); break;
    case 0x24: uart_puts(" (Data Abort,  EL0)"); break;
    case 0x0F: uart_puts(" (SVC AArch64)");      break;
    case 0x22: uart_puts(" (PC alignment)");      break;
    case 0x26: uart_puts(" (SP alignment)");      break;
    default:   uart_puts(" (unknown)");           break;
    }
    uart_puts("\n");

    uart_puts("FAR_EL1 = "); uart_puthex64(far);  uart_puts("\n");
    uart_puts("ELR_EL1 = "); uart_puthex64(regs->elr); uart_puts("\n");
    uart_puts("ISS     = "); uart_puthex64(iss);   uart_puts("\n");

    /* For data aborts, decode DFSC (Data Fault Status Code) */
    if (ec == 0x25 || ec == 0x24) {
        u32 dfsc = iss & 0x3F;
        uart_puts("DFSC    = "); uart_puthex64(dfsc);
        switch (dfsc & ~3u) {
        case 0x04: uart_puts(" (Translation fault)"); break;
        case 0x08: uart_puts(" (Access flag fault)"); break;
        case 0x0C: uart_puts(" (Permission fault)");  break;
        default:   break;
        }
        uart_puts("\n");
    }

    uart_puts("HALT\n");
    while (1) __asm__ volatile("wfe");
}

/*
 * exc_sync_el0_handler — EL0 synchronous exception
 * Will become the syscall dispatcher in Phase 5.
 */
void exc_sync_el0_handler(struct pt_regs *regs, ulong esr)
{
    (void)regs;
    uart_puts("\n*** EL0 SYNC  ESR="); uart_puthex64(esr);
    uart_puts("\n");
    while (1) __asm__ volatile("wfe");
}
