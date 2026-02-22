#ifndef _GIC_H
#define _GIC_H

#include "types.h"

/*
 * ARM GIC v3 driver
 *
 * Linux reference: drivers/irqchip/irq-gic-v3.c
 *
 * GIC v3 components:
 *   GICD  Distributor      — global, routes SPI (INTID ≥ 32)
 *   GICR  Redistributor    — per-CPU, handles SGI (0–15) and PPI (16–31)
 *   CPU IF CPU interface   — accessed via system registers (ICC_*)
 *
 * QEMU 'virt' machine GIC v3 addresses:
 *   GICD base:  0x08000000  (64 KB)
 *   GICR base:  0x080A0000  (128 KB per CPU: RD frame + SGI frame)
 *
 * INTID assignment (ARM IHI 0069):
 *   0 –  15  SGI  Software-Generated Interrupts
 *  16 –  31  PPI  Private Peripheral Interrupts (per-CPU)
 *      INTID 27: virtual timer         (CNTVIRQ)
 *      INTID 30: EL1 phys timer        (CNTPNSIRQ)  ← we use this
 *  32 – 1019  SPI  Shared Peripheral Interrupts
 *      INTID 33: PL011 UART
 */

/* ── MMIO accessors (mirrors asm/io.h readl/writel) ───────────────────── */
#define mmio_r32(addr)       (*(volatile u32 *)(ulong)(addr))
#define mmio_w32(addr, val)  (*(volatile u32 *)(ulong)(addr) = (u32)(val))
#define mmio_r64(addr)       (*(volatile u64 *)(ulong)(addr))
#define mmio_w64(addr, val)  (*(volatile u64 *)(ulong)(addr) = (u64)(val))

/* ── Distributor (GICD) ────────────────────────────────────────────────── */
#define GICD_BASE           0x08000000UL

#define GICD_CTLR           (GICD_BASE + 0x0000)
#define GICD_TYPER          (GICD_BASE + 0x0004)
#define GICD_IGROUPR(n)     (GICD_BASE + 0x0080 + (n)*4)
#define GICD_ISENABLER(n)   (GICD_BASE + 0x0100 + (n)*4)
#define GICD_ICENABLER(n)   (GICD_BASE + 0x0180 + (n)*4)
#define GICD_ICPENDR(n)     (GICD_BASE + 0x0280 + (n)*4)
#define GICD_IPRIORITYR(n)  (GICD_BASE + 0x0400 + (n)*4)
#define GICD_IGRPMODR(n)    (GICD_BASE + 0x0D00 + (n)*4)
#define GICD_IROUTER(n)     (GICD_BASE + 0x6000 + (n)*8)  /* SPI routing */

/* GICD_CTLR bits */
#define GICD_CTLR_RWP       (1u << 31)   /* register write pending */
#define GICD_CTLR_ARE_NS    (1u << 4)    /* affinity routing enable */
#define GICD_CTLR_ENGRP1NS  (1u << 1)    /* enable group 1 non-secure */

/* ── Redistributor (GICR) ──────────────────────────────────────────────── */
#define GICR_BASE           0x080A0000UL  /* CPU0 redistributor */
#define GICR_SGI_BASE       (GICR_BASE + 0x10000UL)   /* SGI/PPI frame */

/* Redistributor frame */
#define GICR_WAKER          (GICR_BASE  + 0x0014)

/* SGI/PPI frame — offsets from GICR_SGI_BASE */
#define GICR_IGROUPR0       (GICR_SGI_BASE + 0x0080)
#define GICR_ISENABLER0     (GICR_SGI_BASE + 0x0100)
#define GICR_ICENABLER0     (GICR_SGI_BASE + 0x0180)
#define GICR_ICPENDR0       (GICR_SGI_BASE + 0x0280)
#define GICR_IPRIORITYR(n)  (GICR_SGI_BASE + 0x0400 + (n)*4)
#define GICR_IGRPMODR0      (GICR_SGI_BASE + 0x0D00)

/* GICR_WAKER bits */
#define GICR_WAKER_PS       (1u << 1)   /* ProcessorSleep: write 0 to wake */
#define GICR_WAKER_CA       (1u << 2)   /* ChildrenAsleep: poll until 0 */

/* ── CPU Interface system registers ────────────────────────────────────── */
/* Encoded as S<op0>_<op1>_C<CRn>_C<CRm>_<op2> (ARM DDI 0487) */
#define ICC_SRE_EL1         "S3_0_C12_C12_5"   /* system reg enable */
#define ICC_PMR_EL1         "S3_0_C4_C6_0"     /* priority mask */
#define ICC_BPR1_EL1        "S3_0_C12_C12_3"   /* binary point */
#define ICC_IAR1_EL1        "S3_0_C12_C12_0"   /* interrupt ack (read INTID) */
#define ICC_EOIR1_EL1       "S3_0_C12_C12_1"   /* end of interrupt */
#define ICC_IGRPEN1_EL1     "S3_0_C12_C12_7"   /* group 1 enable */

#define icc_read(reg)        ({ u64 _v; __asm__ volatile("mrs %0, " reg : "=r"(_v)); _v; })
#define icc_write(reg, val)  __asm__ volatile("msr " reg ", %0\nisb" :: "r"((u64)(val)))

/* ── INTID constants ───────────────────────────────────────────────────── */
#define IRQ_TIMER_PHYS      30    /* EL1 non-secure physical timer (PPI) */
#define IRQ_UART            33    /* PL011 UART (SPI, added in Phase 6)  */
#define INTID_SPURIOUS      1023  /* returned by IAR when no IRQ pending */

/* ── API ───────────────────────────────────────────────────────────────── */
void gic_init(void);
void gic_enable_irq(int intid);
void gic_disable_irq(int intid);
void gic_set_priority(int intid, u8 prio);
int  gic_ack(void);    /* read ICC_IAR1_EL1 → INTID */
void gic_eoi(int intid);  /* write ICC_EOIR1_EL1 */

#endif /* _GIC_H */
