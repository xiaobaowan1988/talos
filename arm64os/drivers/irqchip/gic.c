/*
 * drivers/irqchip/gic.c — ARM GIC v3 driver
 *
 * Linux reference: drivers/irqchip/irq-gic-v3.c
 *   gic_init_bases()   → GICD + GICR init
 *   gic_cpu_init()     → per-CPU redistributor + CPU interface
 *   gic_irq_enable()   → GICR_ISENABLER / GICD_ISENABLER
 *   gic_eoi_irq()      → ICC_EOIR1_EL1
 *
 * GIC v3 initialisation sequence (ARM IHI 0069):
 *
 *   1. Distributor (GICD):
 *      a. Disable: GICD_CTLR = 0, poll RWP=0
 *      b. Set all SPIs to Group 1 NS: GICD_IGROUPRn = 0xFFFFFFFF
 *      c. Set all SPIs to Group 1 modifier 0: GICD_IGRPMODRn = 0
 *      d. Enable affinity routing + Group 1 NS: GICD_CTLR = ARE_NS|Grp1NS
 *
 *   2. Redistributor (GICR), per CPU:
 *      a. Wake: clear GICR_WAKER.ProcessorSleep, poll ChildrenAsleep=0
 *      b. Set SGI/PPI to Group 1 NS: GICR_IGROUPR0 = 0xFFFFFFFF
 *      c. Modifier to Group 1: GICR_IGRPMODR0 = 0
 *
 *   3. CPU Interface (system registers):
 *      a. Enable system register access: ICC_SRE_EL1.SRE = 1
 *      b. Priority mask (allow all): ICC_PMR_EL1 = 0xFF
 *      c. Binary point (all preemption): ICC_BPR1_EL1 = 0
 *      d. Enable Group 1: ICC_IGRPEN1_EL1 = 1
 */

#include "../../include/types.h"
#include "../../include/gic.h"
#include "../../include/uart.h"

/* ── helpers ──────────────────────────────────────────────────────────── */

/* Poll until GICD_CTLR.RWP (Register Write Pending) clears */
static void gicd_wait_rwp(void)
{
    while (mmio_r32(GICD_CTLR) & GICD_CTLR_RWP)
        __asm__ volatile("nop");
}

/* ── Distributor initialisation ───────────────────────────────────────── */

static void gicd_init(void)
{
    u32 typer    = mmio_r32(GICD_TYPER);
    u32 nr_lines = ((typer & 0x1F) + 1) * 32;   /* ITLinesNumber field */
    u32 n;

    uart_puts("GIC:  GICD @ ");
    uart_puthex64(GICD_BASE);
    uart_puts("  lines=");
    uart_putdec(nr_lines);
    uart_puts("\n");

    /* 1a. Disable distributor (needed before reconfiguring) */
    mmio_w32(GICD_CTLR, 0);
    gicd_wait_rwp();

    /*
     * 1b+c. Configure all SPI (INTID 32+) to Group 1 Non-Secure.
     *   GICD_IGROUPRn[bit] = 1  → Group 1
     *   GICD_IGRPMODRn[bit]= 0  → Group 1 NS (not Group 1 S)
     *
     * We skip INTID 0–31 (SGI/PPI) — those live in GICR.
     * Register 0 covers INTID 0–31, so start at n=1.
     *
     * See: gic_dist_config() in irq-gic-common.c
     */
    for (n = 1; n < nr_lines / 32; n++) {
        mmio_w32(GICD_IGROUPR(n),  0xFFFFFFFF);
        mmio_w32(GICD_IGRPMODR(n), 0x00000000);
    }

    /* Default priorities: all SPIs at 0xA0 (medium, below PMR=0xFF) */
    for (n = 8; n < nr_lines / 4; n++)
        mmio_w32(GICD_IPRIORITYR(n), 0xA0A0A0A0);

    /*
     * 1d. Enable distributor with affinity routing (ARE_NS) and Group 1.
     * ARE_NS=1: SPIs routed by GICD_IROUTER, not ITARGETSR.
     * See: gic_dist_init() → writel_relaxed(GICD_CTLR, ...)
     */
    mmio_w32(GICD_CTLR, GICD_CTLR_ARE_NS | GICD_CTLR_ENGRP1NS);
    gicd_wait_rwp();
}

/* ── Redistributor initialisation (CPU 0) ─────────────────────────────── */

static void gicr_init(void)
{
    u32 waker;

    uart_puts("GIC:  GICR @ ");
    uart_puthex64(GICR_BASE);
    uart_puts("\n");

    /*
     * 2a. Wake the redistributor:
     *   Clear ProcessorSleep bit, then poll until ChildrenAsleep=0.
     *
     * See: gic_redist_wait_for_connection() in irq-gic-v3.c
     */
    waker = mmio_r32(GICR_WAKER);
    waker &= ~GICR_WAKER_PS;
    mmio_w32(GICR_WAKER, waker);

    while (mmio_r32(GICR_WAKER) & GICR_WAKER_CA)
        __asm__ volatile("nop");

    /*
     * 2b+c. Set all SGI/PPI to Group 1 Non-Secure.
     *   GICR_IGROUPR0   = 0xFFFFFFFF  (all Group 1)
     *   GICR_IGRPMODR0  = 0x00000000  (Group 1 NS)
     */
    mmio_w32(GICR_IGROUPR0,  0xFFFFFFFF);
    mmio_w32(GICR_IGRPMODR0, 0x00000000);

    /* Default priorities for SGI/PPI: 0xA0 */
    int i;
    for (i = 0; i < 8; i++)
        mmio_w32(GICR_IPRIORITYR(i), 0xA0A0A0A0);

    /* Disable all SGI/PPI (we'll enable individually via gic_enable_irq) */
    mmio_w32(GICR_ICENABLER0, 0xFFFFFFFF);

    /* Clear all pending */
    mmio_w32(GICR_ICPENDR0, 0xFFFFFFFF);
}

/* ── CPU Interface (system registers) ────────────────────────────────── */

static void gicc_init(void)
{
    u64 sre;

    /*
     * 3a. Enable system register access (ICC_SRE_EL1.SRE = 1).
     * Without this, accessing ICC_* registers causes an Undefined trap.
     * See: gic_cpu_sys_reg_init() in irq-gic-v3.c
     */
    sre = icc_read(ICC_SRE_EL1);
    icc_write(ICC_SRE_EL1, sre | 1UL);

    /* Confirm SRE is set (read back) */
    sre = icc_read(ICC_SRE_EL1);
    uart_puts("GIC:  ICC_SRE_EL1 = ");
    uart_puthex64(sre);
    uart_puts(sre & 1 ? " (SRE=1 OK)\n" : " (SRE=0 FAIL)\n");

    /*
     * 3b. Priority mask: 0xFF allows all priorities.
     * ICC_PMR_EL1 is the minimum priority threshold — only interrupts
     * with priority LESS than this value are signalled to the CPU.
     * (Lower numeric value = higher priority in GIC.)
     */
    icc_write(ICC_PMR_EL1, 0xFFUL);

    /*
     * 3c. Binary point = 0: all priority bits used for preemption.
     * See: gic_cpu_init() → write_gicreg(0, ICC_BPR1_EL1)
     */
    icc_write(ICC_BPR1_EL1, 0UL);

    /*
     * 3d. Enable Group 1 interrupts.
     * See: gic_cpu_init() → write_gicreg(1, ICC_IGRPEN1_EL1)
     */
    icc_write(ICC_IGRPEN1_EL1, 1UL);

    uart_puts("GIC:  CPU interface ready (Group1 enabled)\n");
}

/* ── Public API ───────────────────────────────────────────────────────── */

void gic_init(void)
{
    uart_puts("GIC:  initialising GICv3\n");
    gicd_init();
    gicr_init();
    gicc_init();
    uart_puts("GIC:  ready\n");
}

/*
 * gic_enable_irq — enable delivery of INTID to the CPU
 *
 * SGI/PPI (INTID < 32): written to GICR_ISENABLER0
 * SPI     (INTID ≥ 32): written to GICD_ISENABLERn
 *
 * See: gic_irq_enable() in irq-gic-v3.c
 */
void gic_enable_irq(int intid)
{
    if (intid < 32) {
        mmio_w32(GICR_ISENABLER0, 1u << intid);
    } else {
        mmio_w32(GICD_ISENABLER(intid / 32), 1u << (intid % 32));
    }
}

void gic_disable_irq(int intid)
{
    if (intid < 32) {
        mmio_w32(GICR_ICENABLER0, 1u << intid);
    } else {
        mmio_w32(GICD_ICENABLER(intid / 32), 1u << (intid % 32));
    }
}

/*
 * gic_set_priority — set interrupt priority (0=highest, 0xFF=lowest)
 * Only priorities < ICC_PMR_EL1 (0xFF) are delivered; all pass here.
 */
void gic_set_priority(int intid, u8 prio)
{
    if (intid < 32) {
        volatile u8 *p = (volatile u8 *)(GICR_SGI_BASE + 0x0400 + intid);
        *p = prio;
    } else {
        volatile u8 *p = (volatile u8 *)(GICD_BASE + 0x0400 + intid);
        *p = prio;
    }
}

/*
 * gic_ack — read ICC_IAR1_EL1 to acknowledge the pending interrupt
 *
 * Returns the INTID of the highest-priority pending interrupt.
 * Reading IAR also performs priority-drop (lowers active priority level).
 *
 * See: gic_handle_irq() → irqchip_handle_irq() in irq-gic-v3.c
 */
int gic_ack(void)
{
    return (int)(icc_read(ICC_IAR1_EL1) & 0x3FF);
}

/*
 * gic_eoi — write ICC_EOIR1_EL1 to signal end-of-interrupt
 *
 * Deactivates the interrupt in the CPU interface.
 * Must be called after the handler has finished processing.
 *
 * See: gic_eoi_irq() → write_gicreg(d->hwirq, ICC_EOIR1_EL1)
 */
void gic_eoi(int intid)
{
    icc_write(ICC_EOIR1_EL1, (u64)intid);
}
