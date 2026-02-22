#ifndef _TIMER_H
#define _TIMER_H

#include "types.h"

/*
 * ARM generic timer (arch timer)
 *
 * Linux reference: drivers/clocksource/arm_arch_timer.c
 *
 * The ARM generic timer provides:
 *   - A 64-bit up-counting physical counter (CNTPCT_EL0)
 *   - Per-CPU compare/countdown registers for generating interrupts
 *   - A firmware-defined frequency (CNTFRQ_EL0, typically 62.5 MHz on QEMU)
 *
 * Registers:
 *   CNTFRQ_EL0     timer frequency in Hz (read-only, set by firmware)
 *   CNTPCT_EL0     current physical count (64-bit, monotonic)
 *   CNTP_CTL_EL0   control: ENABLE[0], IMASK[1], ISTATUS[2]
 *   CNTP_TVAL_EL0  countdown value: fires when == 0
 *   CNTP_CVAL_EL0  absolute compare value (alternative to TVAL)
 *
 * IRQ: INTID 30 (PPI, EL1 non-secure physical timer)
 */

#define HZ          100    /* timer ticks per second */

void  timer_init(void);
ulong timer_get_ticks(void);   /* total tick count since boot */
ulong timer_get_us(void);      /* microseconds since boot */

/* Called by IRQ dispatch when INTID 30 fires */
void timer_irq_handler(int irq, void *data);

#endif /* _TIMER_H */
