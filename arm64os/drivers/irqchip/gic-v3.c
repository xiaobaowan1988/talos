/* SPDX-License-Identifier: GPL-2.0 */
/*
 * arm64os/drivers/irqchip/gic-v3.c
 *
 * ARM GIC v3 中断控制器驱动
 *
 * 参考：drivers/irqchip/irq-gic-v3.c
 *       include/linux/irqchip/arm-gic-v3.h
 *       ARM Generic Interrupt Controller Architecture Specification v3/v4
 *
 * Phase 3 简化实现（单CPU，无ITS，无NUMA）：
 *   - 初始化 Distributor（GICD）：使能 Affinity Routing + Group 1 NS
 *   - 初始化 Redistributor（GICR）：唤醒 CPU0
 *   - 初始化 CPU 接口：通过系统寄存器（SRE模式）配置优先级掩码和使能
 *
 * QEMU virt machine MMIO 地址：
 *   GICD_BASE = 0x08000000
 *   GICR_BASE = 0x080A0000（CPU0 RD帧）
 *   GICR_SGI_BASE = 0x080B0000（CPU0 SGI帧 = RD帧 + 0x10000）
 */

#include <linux/types.h>
#include <linux/io.h>

/* ============================================================
 * GICD 寄存器偏移（相对 GICD_BASE）
 * 参考：include/linux/irqchip/arm-gic-v3.h
 * ============================================================ */
#define GICD_CTLR           0x0000U /* 分发器控制 */
#define GICD_TYPER          0x0004U /* 类型寄存器（中断数量等）*/
#define GICD_IGROUPR        0x0080U /* 中断分组（每bit一个中断；word0=SGI,word1+=SPI）*/
#define GICD_ISENABLER      0x0100U /* SPI 使能设置（每bit一个中断）*/
#define GICD_ICENABLER      0x0180U /* SPI 使能清除 */
#define GICD_IPRIORITYR     0x0400U /* 中断优先级（每字节一个中断）*/
#define GICD_IGRPMODR       0x0D00U /* 中断分组修改器（配合IGROUPR决定Group 1 NS/S）*/
#define GICD_IROUTER        0x6000U /* SPI 路由（64位/中断，ARE_NS模式）*/

/* GICD_CTLR bit 定义 */
#define GICD_CTLR_ENABLE_G1NS   (1U << 1)   /* Group 1 Non-Secure 使能 */
#define GICD_CTLR_ARE_NS        (1U << 4)   /* Affinity Routing Enable (NS) */
#define GICD_CTLR_RWP           (1U << 31)  /* Read-Write-Pending（操作进行中）*/

/* ============================================================
 * GICR 寄存器偏移
 *
 * RD帧（相对 GICR_RD_BASE = 0x080A0000）：
 * ============================================================ */
#define GICR_TYPER          0x0008U /* 类型寄存器（包含 CPU affinity）*/
#define GICR_WAKER          0x0014U /* 唤醒控制（ProcessorSleep/ChildrenAsleep）*/

/*
 * SGI帧（相对 GICR_SGI_BASE = GICR_RD_BASE + 0x10000）：
 * 管理 SGI（0-15）和 PPI（16-31）的使能与优先级
 */
#define GICR_SGI_OFFSET     0x10000U            /* SGI帧相对RD帧的偏移 */
#define GICR_IGROUPR0       0x0080U             /* SGI/PPI 中断分组 */
#define GICR_ISENABLER0     0x0100U             /* SGI/PPI 使能设置 */
#define GICR_ICENABLER0     0x0180U             /* SGI/PPI 使能清除 */
#define GICR_IPRIORITYR0    0x0400U             /* SGI/PPI 优先级（每字节一个）*/
#define GICR_IGRPMODR0      0x0D00U             /* SGI/PPI 中断分组修改器 */

/* GICR_WAKER bit 定义 */
#define GICR_WAKER_ProcessorSleep   (1U << 1)   /* 写0唤醒，写1休眠 */
#define GICR_WAKER_ChildrenAsleep   (1U << 2)   /* 只读：1=子组件已休眠 */

/* ============================================================
 * QEMU virt machine GIC v3 MMIO 地址
 * ============================================================ */
#define GICD_BASE_ADDR  0x08000000UL    /* Distributor */
#define GICR_BASE_ADDR  0x080A0000UL    /* CPU0 Redistributor RD帧 */

/* ============================================================
 * gicv3_init - 初始化 GIC v3
 *
 * 调用时机：start_kernel() 中，MMU 和 Buddy 初始化完成后，
 *           使能中断（daifclr）之前。
 *
 * 参考：drivers/irqchip/irq-gic-v3.c: gic_init_bases()
 *                                      gic_cpu_init()
 * ============================================================ */
void gicv3_init(void)
{
    volatile void *gicd = (volatile void *)GICD_BASE_ADDR;
    volatile void *gicr = (volatile void *)GICR_BASE_ADDR;
    volatile void *gicr_sgi = (volatile void *)(GICR_BASE_ADDR + GICR_SGI_OFFSET);
    int i;

    /* ---- Step 1: 初始化 Distributor ---- */

    /*
     * 1.1 禁用 Distributor：写 0 到 GICD_CTLR。
     * 等待 RWP（Read-Write-Pending）位清零，确保操作完成。
     *
     * 参考：irq-gic-v3.c: gic_dist_init()
     */
    writel(0, gicd + GICD_CTLR);
    while (readl(gicd + GICD_CTLR) & GICD_CTLR_RWP)
        ;

    /*
     * 1.1b 将所有 SPI（IRQ 32-1019）配置为 Group 1 Non-Secure。
     * GICD_IGROUPR  每bit=1 → Group 1（非Secure Group 0）
     * GICD_IGRPMODR 每bit=0 → Non-Secure（配合 IGROUPR=1 = Group 1 NS）
     *
     * GICv3 复位默认：IGROUPR=0（Group 0，触发 FIQ），我们必须显式设置。
     * word 0 管理 SGI/PPI（IRQ 0-31），由 Redistributor 负责，此处跳过。
     * word 1-31 管理 SPI（IRQ 32-1023）。
     *
     * 参考：irq-gic-v3.c: gic_dist_init()
     */
    for (i = 1; i < 32; i++) {
        writel(0xFFFFFFFFU, gicd + GICD_IGROUPR  + i * 4);
        writel(0x00000000U, gicd + GICD_IGRPMODR + i * 4);
    }

    /*
     * 1.2 使能 Affinity Routing（ARE_NS）+ Group 1 Non-Secure。
     * ARE_NS 是 GICv3 新特性，允许按 CPU affinity 路由 SPI 中断。
     *
     * 参考：irq-gic-v3.c: gic_dist_init():
     *   writel_relaxed(GICD_CTLR_ARE_NS | GICD_CTLR_ENABLE_G1A |
     *                  GICD_CTLR_ENABLE_G1, base + GICD_CTLR);
     */
    writel(GICD_CTLR_ARE_NS | GICD_CTLR_ENABLE_G1NS, gicd + GICD_CTLR);
    while (readl(gicd + GICD_CTLR) & GICD_CTLR_RWP)
        ;

    /*
     * 1.3 将所有 SPI（IRQ 32-1019）路由到 CPU0。
     * 在 ARE_NS 模式下，GICD_IROUTER 为 64 位寄存器，
     * 写入 0 表示 affinity 为 0.0.0.0（CPU0）。
     *
     * 参考：irq-gic-v3.c: gic_dist_init()
     */
    for (i = 32; i < 1020; i++)
        writeq(0, gicd + GICD_IROUTER + i * 8);

    /*
     * 1.4 设置所有 SPI 优先级为低优先级（0xA0），并禁用所有 SPI。
     * Linux 内核使用 0xa0 作为默认低优先级值。
     * GICD_IPRIORITYR 每 4 字节管理 4 个中断的优先级。
     * GICD_ICENABLER 每位对应一个中断，写 1 禁用。
     */
    for (i = 32 / 4; i < 1020 / 4; i++)
        writel(0xA0A0A0A0U, gicd + GICD_IPRIORITYR + i * 4);
    for (i = 32 / 32; i < (1020 + 31) / 32; i++)
        writel(0xFFFFFFFFU, gicd + GICD_ICENABLER + i * 4);

    /* ---- Step 2: 初始化 Redistributor（CPU0）---- */

    /*
     * 2.1 唤醒 Redistributor：清除 GICR_WAKER.ProcessorSleep（bit 1）。
     * CPU 上电时 Redistributor 处于休眠状态，需要显式唤醒。
     * 等待 ChildrenAsleep（bit 2）清零，表示唤醒完成。
     *
     * 参考：irq-gic-v3.c: gic_redist_wait_for_rwp() / gic_cpu_init()
     */
    {
        u32 waker = readl(gicr + GICR_WAKER);
        waker &= ~GICR_WAKER_ProcessorSleep;
        writel(waker, gicr + GICR_WAKER);
        while (readl(gicr + GICR_WAKER) & GICR_WAKER_ChildrenAsleep)
            ;
    }

    /*
     * 2.1b 将所有 SGI/PPI（IRQ 0-31）配置为 Group 1 Non-Secure。
     * GICv3 复位默认：GICR_IGROUPR0=0（Group 0，触发FIQ），
     * Virtual Timer PPI #27 因此默认走 FIQ 而非 IRQ，导致中断无法到达。
     * 写入 0xFFFFFFFF 使所有 SGI/PPI 变为 Group 1，
     * 配合 IGRPMODR0=0 → Group 1 Non-Secure → 触发 IRQ（EL1 可接收）。
     *
     * 参考：irq-gic-v3.c: gic_cpu_init()
     */
    writel(0xFFFFFFFFU, gicr_sgi + GICR_IGROUPR0);
    writel(0x00000000U, gicr_sgi + GICR_IGRPMODR0);

    /*
     * 2.2 初始化 SGI/PPI（IRQ 0-31）：设置低优先级，禁用所有。
     * GICR_IPRIORITYR0 管理 IRQ 0-31 的优先级（每字节一个）。
     * GICR_ICENABLER0 禁用所有 SGI/PPI。
     */
    for (i = 0; i < 32 / 4; i++)
        writel(0xA0A0A0A0U, gicr_sgi + GICR_IPRIORITYR0 + i * 4);
    writel(0xFFFFFFFFU, gicr_sgi + GICR_ICENABLER0);

    /* ---- Step 3: 初始化 CPU 接口（通过系统寄存器）---- */

    /*
     * 3.1 使能系统寄存器接口（ICC_SRE_EL1）。
     * GICv3 支持两种 CPU 接口访问方式：内存映射（兼容GICv2）和系统寄存器。
     * 我们使用系统寄存器方式（SRE=1）。
     * 同时设置 DFB（Disable FIQ bypass）和 DIB（Disable IRQ bypass）。
     *   ICC_SRE_EL1.SRE = bit 0 = 1
     *   ICC_SRE_EL1.DFB = bit 1 = 1
     *   ICC_SRE_EL1.DIB = bit 2 = 1
     * → 写入 7（0b111）
     *
     * 参考：irq-gic-v3.c: gic_cpu_sys_reg_init()
     */
    __asm__ volatile("msr ICC_SRE_EL1, %0" :: "r"(7UL));
    __asm__ volatile("isb");  /* 确保 SRE 设置对后续系统寄存器访问生效 */

    /*
     * 3.2 设置优先级掩码（ICC_PMR_EL1 = 0xFF）。
     * PMR 定义了 CPU 能接收的最低优先级阈值。
     * 0xFF 表示接受所有优先级的中断（0 = 最高优先级，0xFF = 最低）。
     *
     * 参考：irq-gic-v3.c: gic_cpu_init()
     */
    __asm__ volatile("msr ICC_PMR_EL1, %0" :: "r"(0xFFUL));

    /*
     * 3.3 设置 Binary Point Register（ICC_BPR1_EL1 = 0）。
     * 控制优先级分组/子分组的分割点。0 = 不分组（所有8位用于优先级）。
     */
    __asm__ volatile("msr ICC_BPR1_EL1, %0" :: "r"(0UL));

    /*
     * 3.4 使能 Group 1 中断（ICC_IGRPEN1_EL1 = 1）。
     * 最终开关：允许 CPU 接收 Group 1 中断。
     * 注意：此时 DAIF.I 位仍可能屏蔽 IRQ，需 daifclr 后才真正响应。
     *
     * 参考：irq-gic-v3.c: gic_cpu_init()
     */
    __asm__ volatile("msr ICC_IGRPEN1_EL1, %0" :: "r"(1UL));
    __asm__ volatile("isb");
}

/* ============================================================
 * gicv3_enable_irq - 使能 SPI 中断（IRQ >= 32）
 *
 * @irq: SPI 中断号（32-1019）
 *
 * 参考：irq-gic-v3.c: gic_unmask_irq()
 * ============================================================ */
void gicv3_enable_irq(unsigned int irq)
{
    volatile void *gicd = (volatile void *)GICD_BASE_ADDR;

    if (irq < 32 || irq >= 1020)
        return;

    writel(1U << (irq % 32), gicd + GICD_ISENABLER + (irq / 32) * 4);
}

/* ============================================================
 * gicv3_enable_ppi - 使能 PPI/SGI 中断（IRQ < 32）
 *
 * @irq: PPI 或 SGI 中断号（0-31）
 *       本阶段主要用于使能 Virtual Timer PPI #27。
 *
 * PPI/SGI 使能寄存器在 Redistributor 的 SGI 帧中，
 * 不在 Distributor 中。
 *
 * 参考：irq-gic-v3.c: gic_unmask_irq()（通过 flow_handler）
 * ============================================================ */
void gicv3_enable_ppi(unsigned int irq)
{
    volatile void *gicr_sgi =
        (volatile void *)(GICR_BASE_ADDR + GICR_SGI_OFFSET);

    if (irq >= 32)
        return;

    writel(1U << irq, gicr_sgi + GICR_ISENABLER0);
}
