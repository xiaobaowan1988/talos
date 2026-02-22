#ifndef _IRQ_H
#define _IRQ_H

#include "types.h"

/*
 * IRQ framework
 *
 * Linux reference: include/linux/interrupt.h, kernel/irq/manage.c
 *
 * Simplified model:
 *   - Fixed table of MAX_IRQS descriptors (no dynamic allocation yet)
 *   - request_irq() registers a handler for a given INTID
 *   - irq_dispatch() called by the GIC handler to run the right callback
 */

#define MAX_IRQS    256

typedef void (*irq_handler_t)(int irq, void *data);

struct irq_desc {
    irq_handler_t  handler;
    void          *data;
    const char    *name;
    ulong          count;   /* number of times this IRQ fired */
};

extern struct irq_desc irq_table[MAX_IRQS];

void irq_init(void);
int  request_irq(int irq, irq_handler_t handler, void *data, const char *name);
void irq_dispatch(int irq);

/*
 * irq_enable / irq_disable — toggle PSTATE.I (IRQ mask bit)
 *
 * daifclr #2 → clear I bit → IRQs enabled
 * daifset #2 → set   I bit → IRQs disabled
 *
 * See: arch/arm64/include/asm/irqflags.h
 */
static inline void irq_enable(void)
{
    __asm__ volatile("msr daifclr, #2\nisb" ::: "memory");
}

static inline void irq_disable(void)
{
    __asm__ volatile("msr daifset, #2\nisb" ::: "memory");
}

/* pt_regs — mirrors arch/arm64/include/asm/ptrace.h */
struct pt_regs {
    ulong regs[30];    /* x0–x29 */
    ulong lr;          /* x30    */
    ulong _pad;
    ulong elr;         /* ELR_EL1 — return PC */
    ulong spsr;        /* SPSR_EL1 */
};

/* Called from entry.S exception vectors */
void exc_sync_el1_handler(struct pt_regs *regs, ulong esr, ulong far);
void exc_irq_el1_handler(struct pt_regs *regs);
void exc_sync_el0_handler(struct pt_regs *regs, ulong esr);

#endif /* _IRQ_H */
