# Phase 3：GIC v3 中断控制器 + Generic Timer

## 知识来源总览

- **ARM GIC v3 规范 (IHI 0069)**：约 45%（Distributor/Redistributor/CPU Interface 寄存器）
- **ARMv8-A ARM Timer 章节**：约 25%（CNTV_TVAL_EL0、CNTV_CTL_EL0、CNTFRQ_EL0）
- **QEMU virt 平台地址映射**：约 15%（GIC 基地址、GICR 偏移）
- **Phase 3 文档**：约 15%

## GIC v3 架构

ARM 的 GIC（Generic Interrupt Controller）v3 有三个主要组件：

**Distributor（GICD）**：全局唯一，管理所有 SPI（Shared Peripheral Interrupt）。基地址 0x08000000（QEMU virt）。

**Redistributor（GICR）**：每个 CPU 核心一个，管理 PPI（Private Peripheral Interrupt）和 SGI（Software Generated Interrupt）。基地址 0x080A0000。

**CPU Interface（ICC）**：通过系统寄存器访问（ICC_*_EL1），不走 MMIO。处理中断的确认（IAR）和完成（EOIR）。

### 为什么分三级？

单核系统只需要一个中断控制器。但多核系统中：
- 某些中断是全局的（网卡中断 → 任何核都能处理）→ Distributor 管理
- 某些中断是核心私有的（Timer → 只有当前核心关心）→ Redistributor 管理
- 中断的确认/完成是每个核心独立的 → CPU Interface 管理

## GICD 初始化

```c
#define GICD_BASE       0x08000000UL
#define GICD_CTLR       (GICD_BASE + 0x000)
#define GICD_TYPER      (GICD_BASE + 0x004)
#define GICD_ISENABLER(n) (GICD_BASE + 0x100 + 4*(n))
#define GICD_IPRIORITYR(n) (GICD_BASE + 0x400 + 4*(n))
#define GICD_ITARGETSR(n)  (GICD_BASE + 0x800 + 4*(n))

void gic_dist_init(void) {
    writel(0, GICD_CTLR);              /* 先禁用 */
    /* ... 配置各中断的优先级、目标核心 ... */
    writel(GICD_CTLR_ENABLE, GICD_CTLR); /* 最后启用 */
}
```

**偏移来源：GIC v3 spec Table 12-1**。

`GICD_CTLR`（偏移 0x000）：Distributor Control Register。bit 0 = EnableGrp1（使能 Group 1 中断）。

`GICD_TYPER`（偏移 0x004）：Type Register。bit[4:0] = ITLinesNumber，表示支持的最大中断号 = 32 × (N+1)。

`GICD_ISENABLER`（偏移 0x100 起）：Interrupt Set-Enable Register。每位控制一个中断号的使能。写 1 使能，写 0 无效（使用 ICENABLER 禁用）。

**为什么先禁用再配置？** 配置过程中如果 Distributor 是使能的，可能在配置到一半时收到中断，此时中断路由配置不完整，行为未定义。

## GICR 初始化

```c
#define GICR_BASE       0x080A0000UL
#define GICR_SGI_BASE   (GICR_BASE + 0x10000)  /* SGI_base 偏移 */
```

**关键细节：SGI_base 在 GICR_BASE + 0x10000**。

GIC v3 的 Redistributor 有两个 64KB 帧：
- GICR_RD_base（偏移 0x0000）：控制和状态寄存器
- GICR_SGI_base（偏移 0x10000）：SGI/PPI 的使能和优先级寄存器

PPI 和 SGI 的 ISENABLER、IPRIORITYR 等寄存器在 SGI_base 帧中，不在 RD_base 帧中。这是初学者容易犯的错误——如果偏移量少了 0x10000，写入的是错误的地址。

## ICC 系统寄存器

```c
/* 使能 CPU Interface */
static inline void gic_cpu_init(void) {
    /* ICC_SRE_EL1: System Register Enable */
    u64 sre;
    asm volatile("mrs %0, " ICC_SRE_EL1 : "=r"(sre));
    sre |= 1;  /* SRE bit: 使用系统寄存器而非MMIO */
    asm volatile("msr " ICC_SRE_EL1 ", %0" :: "r"(sre));
    asm volatile("isb");

    /* ICC_PMR_EL1: Priority Mask = 0xFF（允许所有优先级）*/
    asm volatile("msr " ICC_PMR_EL1 ", %0" :: "r"(0xFF));

    /* ICC_CTLR_EL1: EOImode=0（写EOIR同时deactivate）*/

    /* ICC_IGRPEN1_EL1: Enable Group 1 interrupts */
    asm volatile("msr " ICC_IGRPEN1_EL1 ", %0" :: "r"(1));
    asm volatile("isb");
}
```

**来源：GIC v3 spec Chapter 12, System register interface**。

GIC v3 的 CPU Interface 从 MMIO（GIC v2）改为系统寄存器访问（`mrs`/`msr`），减少了内存总线开销——中断确认（读 IAR）在热路径上，系统寄存器比 MMIO 快 10-20 个周期。

## Generic Timer

```c
#define TIMER_IRQ  27    /* PPI #27 = Virtual Timer */

void timer_init(unsigned int interval_ms) {
    /* 读取频率 */
    u64 freq;
    asm volatile("mrs %0, cntfrq_el0" : "=r"(freq));

    /* 设置倒计时值 */
    u64 tval = freq * interval_ms / 1000;
    asm volatile("msr cntv_tval_el0, %0" :: "r"(tval));

    /* 使能定时器 */
    asm volatile("msr cntv_ctl_el0, %0" :: "r"(1));
}
```

**来源：ARMv8-A ARM, D11 Generic Timer**。

`CNTFRQ_EL0`：计数器频率。QEMU virt = 62,500,000 Hz（62.5MHz）。

`CNTV_TVAL_EL0`：Virtual Timer Value。设为 N 后，计数器倒数到 0 时触发中断。

`CNTV_CTL_EL0`：Virtual Timer Control。bit 0 = ENABLE，bit 1 = IMASK（中断屏蔽），bit 2 = ISTATUS（中断状态，只读）。

**PPI #27**：ARM 规范固定 Virtual Timer 使用 PPI 27（中断号 = 16 + 27 = 43，因为前 16 个是 SGI）。Physical Timer 使用 PPI 30。

## 中断处理流程

```c
void irq_handler(void) {
    /* 1. 读取中断号（自动确认）*/
    u32 iar;
    asm volatile("mrs %0, " ICC_IAR1_EL1 : "=r"(iar));
    u32 irq = iar & 0x3FF;

    /* 2. 处理中断 */
    if (irq == TIMER_IRQ) {
        timer_handler();
    }

    /* 3. 写 EOIR 完成中断 */
    asm volatile("msr " ICC_EOIR1_EL1 ", %0" :: "r"(iar));
}
```

**IAR（Interrupt Acknowledge Register）读取**：返回当前最高优先级的 pending 中断号。读取 IAR 的副作用是将中断从 pending 变为 active（确认中断）。

**EOIR（End of Interrupt Register）写入**：标记中断处理完成。GIC 将中断从 active 变为 inactive，允许同优先级的下一个中断触发。

**IAR → handler → EOIR 的顺序是 GIC 规范强制要求的**。如果忘记写 EOIR，该优先级的中断永远不会再触发（GIC 认为上一个中断仍在处理中）。
