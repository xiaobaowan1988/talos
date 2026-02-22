/*
 * drivers/clocksource/timer.c — ARM generic timer driver
 *
 * Linux reference: drivers/clocksource/arm_arch_timer.c
 *   arch_timer_of_init()       → probe from device tree
 *   arch_timer_register()      → register clocksource + clockevent
 *   arch_timer_handler_phys()  → IRQ handler (physical timer)
 *   arch_timer_set_next_event()→ CNTP_TVAL_EL0 = cycles
 *
 * ARM generic timer provides:
 *   1. clocksource — high-resolution monotonic counter (CNTPCT_EL0)
 *   2. clockevent  — per-CPU periodic interrupt (INTID 30)
 *
 * Registers (accessed via system registers):
 *   CNTFRQ_EL0     read-only frequency register (set by firmware)
 *   CNTPCT_EL0     64-bit up-counting physical counter
 *   CNTP_CTL_EL0   [0]=ENABLE [1]=IMASK [2]=ISTATUS (read-only status)
 *   CNTP_TVAL_EL0  countdown: write N → fires after N counter ticks
 *   CNTP_CVAL_EL0  absolute compare: fires when CNTPCT_EL0 >= CVAL
 *
 * We use TVAL (countdown) for simplicity; Linux uses CVAL for accuracy
 * (avoids drift from interrupt latency).
 */

#include "../../include/types.h"
#include "../../include/timer.h"
#include "../../include/gic.h"
#include "../../include/irq.h"
#include "../../include/uart.h"

/* ── sysreg helpers (mirrors arch/arm64/include/asm/sysreg.h) ─────────── */
#define read_sysreg(r)       ({ u64 _v; __asm__ volatile("mrs %0, " #r : "=r"(_v)); _v; })
#define write_sysreg(v, r)   __asm__ volatile("msr " #r ", %0\nisb" :: "r"((u64)(v)))

/* CNTP_CTL_EL0 bits */
#define ARCH_TIMER_CTRL_ENABLE  (1u << 0)  /* timer enable */
#define ARCH_TIMER_CTRL_IMASK   (1u << 1)  /* interrupt mask (1=masked) */
#define ARCH_TIMER_CTRL_ISTATUS (1u << 2)  /* condition met (read-only) */

/* ── state ────────────────────────────────────────────────────────────── */
static u64   timer_freq;      /* CNTFRQ_EL0 — ticks per second */
static u64   tick_period;     /* counter ticks per HZ */
static ulong total_ticks;     /* monotonic tick counter */

/* ── clocksource ──────────────────────────────────────────────────────── */

/*
 * timer_get_ticks — return elapsed jiffies since boot
 * Mirrors jiffies in kernel/time/jiffies.c (but just our tick counter).
 */
ulong timer_get_ticks(void)
{
    return total_ticks;
}

/*
 * timer_get_us — return microseconds since boot
 * Uses CNTPCT_EL0 (hardware counter) for sub-tick resolution.
 * Mirrors clocksource_cyc2ns() in kernel/time/clocksource.c
 */
ulong timer_get_us(void)
{
    u64 count = read_sysreg(cntpct_el0);
    return (ulong)(count / (timer_freq / 1000000UL));
}

/* ── clockevent (IRQ handler) ─────────────────────────────────────────── */

/*
 * timer_irq_handler — called by irq_dispatch when INTID 30 fires
 *
 * Mirrors arch_timer_handler_phys() in arm_arch_timer.c:
 *   1. Reload TVAL (reschedule next event)
 *   2. Increment jiffies / do_timer()
 *   3. update_process_times() → scheduler tick (Phase 4)
 */
void timer_irq_handler(int irq, void *data)
{
    (void)irq; (void)data;

    /*
     * Reload countdown: write tick_period to CNTP_TVAL_EL0.
     * This schedules the next IRQ tick_period counter ticks from now.
     *
     * Note: Linux uses CNTP_CVAL instead to avoid cumulative drift:
     *   CVAL += tick_period  (absolute, not relative)
     * We use TVAL for simplicity; Phase 4 will switch to CVAL.
     */
    write_sysreg(tick_period, cntp_tval_el0);

    total_ticks++;

    /* Print a heartbeat every second (every HZ ticks) */
    if (total_ticks % (ulong)HZ == 0) {
        uart_puts("tick: ");
        uart_putdec(total_ticks / HZ);
        uart_puts("s  (");
        uart_putdec(total_ticks);
        uart_puts(" ticks, us=");
        uart_putdec(timer_get_us());
        uart_puts(")\n");
    }
}

/* ── initialisation ───────────────────────────────────────────────────── */

/*
 * timer_init — set up the ARM generic timer and register its IRQ
 *
 * Mirrors arch_timer_register() → clockevents_config_and_register()
 */
void timer_init(void)
{
    /*
     * Read the timer frequency from CNTFRQ_EL0.
     * On QEMU virt, the firmware (QEMU itself) sets this to 62,500,000 Hz
     * (62.5 MHz). Real hardware varies (24 MHz, 100 MHz, etc.).
     *
     * See: arch_timer_get_cntfrq() in arm_arch_timer.c
     */
    timer_freq   = read_sysreg(cntfrq_el0);
    tick_period  = timer_freq / HZ;
    total_ticks  = 0;

    uart_puts("TMR:  CNTFRQ_EL0 = ");
    uart_putdec(timer_freq);
    uart_puts(" Hz  tick_period = ");
    uart_putdec(tick_period);
    uart_puts(" counts\n");

    /* Set timer priority to 0x80 (slightly above default 0xA0) */
    gic_set_priority(IRQ_TIMER_PHYS, 0x80);

    /* Register our handler in the IRQ table */
    request_irq(IRQ_TIMER_PHYS, timer_irq_handler, NULL, "arm-arch-timer");

    /* Enable INTID 30 (PPI) in the redistributor */
    gic_enable_irq(IRQ_TIMER_PHYS);

    /*
     * Program the first tick:
     *   CNTP_TVAL_EL0 = tick_period → IRQ fires after tick_period counts
     *   CNTP_CTL_EL0  = ENABLE (bit 0), IMASK=0 (IRQ not masked)
     *
     * See: arch_timer_set_next_event_phys() → write_sysreg(evt, cntp_tval_el0)
     *      arch_timer_enable()              → write_sysreg(ctrl, cntp_ctl_el0)
     */
    write_sysreg(tick_period, cntp_tval_el0);
    write_sysreg(ARCH_TIMER_CTRL_ENABLE, cntp_ctl_el0);

    uart_puts("TMR:  armed at ");
    uart_putdec(HZ);
    uart_puts(" Hz (INTID ");
    uart_putdec(IRQ_TIMER_PHYS);
    uart_puts(")\n");
}
