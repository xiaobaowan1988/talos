# Phase 3：GIC v3中断控制器 + ARM通用计时器

## 参考内核文件

```
drivers/irqchip/irq-gic-v3.c        # GIC v3驱动（主要实现）
drivers/irqchip/irq-gic-v3-its.c    # ITS（中断转发服务，可简化跳过）
include/linux/irqchip/arm-gic-v3.h  # GIC v3寄存器定义
drivers/clocksource/arm_arch_timer.c # ARM通用计时器驱动
include/clocksource/arm_arch_timer.h # 计时器寄存器定义
arch/arm64/kernel/entry.S           # 中断入口（el1h_irq, el0_irq）
```

---

## 3.1 GIC v3架构概览

GIC v3由两部分组成：
- **Distributor（GICD）**：全局，管理所有中断的使能/优先级/路由，MMIO地址一般为 `0x08000000`
- **Redistributor（GICR）**：每CPU一个，管理PPI和SGI，通常在 `0x080A0000`

中断分类：
```
SGI  (0-15)   : Software Generated Interrupts，核间通信
PPI  (16-31)  : Per-CPU Private Interrupts，如arch timer (PPI #30)
SPI  (32-1019): Shared Peripheral Interrupts，设备中断
```

## 3.2 GIC v3寄存器映射（参考 arm-gic-v3.h）

```c
/* GICD寄存器偏移（参考 include/linux/irqchip/arm-gic-v3.h）*/
#define GICD_CTLR           0x0000  /* 分发器控制 */
#define GICD_TYPER          0x0004  /* 类型寄存器（中断数量等）*/
#define GICD_ISENABLER      0x0100  /* SPI中断使能设置（每位一个中断）*/
#define GICD_ICENABLER      0x0180  /* SPI中断使能清除 */
#define GICD_ISPENDR        0x0200  /* pending设置 */
#define GICD_IPRIORITYR     0x0400  /* 中断优先级（每字节一个中断）*/
#define GICD_ICFGR          0x0C00  /* 边沿/电平触发配置 */
#define GICD_IROUTER        0x6000  /* SPI路由寄存器（v3新增）*/

/* GICD_CTLR bit定义 */
#define GICD_CTLR_ENABLE_G0     (1 << 0)   /* Group 0 使能 */
#define GICD_CTLR_ENABLE_G1NS   (1 << 1)   /* Group 1 NS 使能 */
#define GICD_CTLR_ARE_NS        (1 << 4)   /* Affinity Routing Enable */

/* GICR寄存器偏移 */
#define GICR_TYPER          0x0008  /* 类型寄存器 */
#define GICR_WAKER          0x0014  /* 唤醒寄存器（CPU上电/下电）*/
#define GICR_ISENABLER0     0x0100  /* SGI/PPI使能（相对GICR_SGI_BASE）*/
#define GICR_ICENABLER0     0x0180
#define GICR_IPRIORITYR0    0x0400

/* System registers（ICC_*）直接通过MSR/MRS访问 */
// ICC_SRE_EL1:   使能系统寄存器接口（必须先设置）
// ICC_PMR_EL1:   优先级掩码（0xFF = 允许所有中断）
// ICC_BPR0_EL1:  Binary Point Register
// ICC_IGRPEN1_EL1: Group 1中断全局使能
// ICC_IAR1_EL1:  Interrupt Acknowledge（读取中断号并标记Active）
// ICC_EOIR1_EL1: End of Interrupt（写中断号标记处理完成）
```

## 3.3 GIC v3初始化流程

```c
/* 参考 drivers/irqchip/irq-gic-v3.c : gic_init_bases() */

void gicv3_init(void *gicd_base, void *gicr_base) {
    /* Step 1: Distributor初始化 */
    /* 1.1 禁用Distributor */
    writel(0, gicd_base + GICD_CTLR);
    while (readl(gicd_base + GICD_CTLR) & GICD_CTLR_RWP);

    /* 1.2 使能Affinity Routing（GICv3特性）*/
    writel(GICD_CTLR_ARE_NS | GICD_CTLR_ENABLE_G1NS,
           gicd_base + GICD_CTLR);

    /* 1.3 将所有SPI路由到CPU0（Affinity: 0.0.0.0）*/
    for (int i = 32; i < 1020; i++) {
        writeq(0, gicd_base + GICD_IROUTER + i * 8);
    }

    /* 1.4 将所有SPI设置为最低优先级，禁用 */
    for (int i = 32/4; i < 1020/4; i++) {
        writel(0xA0A0A0A0, gicd_base + GICD_IPRIORITYR + i*4);
    }
    for (int i = 32/32; i < 1020/32; i++) {
        writel(0xFFFFFFFF, gicd_base + GICD_ICENABLER + i*4);
    }

    /* Step 2: Redistributor初始化（每CPU）*/
    /* 2.1 将CPU从Sleep状态唤醒 */
    uint32_t waker = readl(gicr_base + GICR_WAKER);
    waker &= ~(1 << 1);  /* 清除 ProcessorSleep */
    writel(waker, gicr_base + GICR_WAKER);
    while (readl(gicr_base + GICR_WAKER) & (1 << 2));

    /* Step 3: CPU接口初始化（通过系统寄存器）*/
    /* 3.1 使能系统寄存器接口 */
    asm volatile("msr ICC_SRE_EL1, %0" :: "r"(7UL));
    asm volatile("isb");

    /* 3.2 设置优先级掩码（允许所有中断）*/
    asm volatile("msr ICC_PMR_EL1, %0" :: "r"(0xFFUL));

    /* 3.3 设置Binary Point */
    asm volatile("msr ICC_BPR1_EL1, %0" :: "r"(0UL));

    /* 3.4 使能Group 1中断 */
    asm volatile("msr ICC_IGRPEN1_EL1, %0" :: "r"(1UL));
    asm volatile("isb");
}

/* 中断处理主循环（在entry.S的IRQ handler中调用）*/
void handle_irq(void) {
    uint64_t irqnr;
    asm volatile("mrs %0, ICC_IAR1_EL1" : "=r"(irqnr));

    if (irqnr < 1020) {
        irq_handlers[irqnr]();
        asm volatile("msr ICC_EOIR1_EL1, %0" :: "r"(irqnr));
        asm volatile("isb");
    }
}

/* 使能某个SPI中断 */
void gicv3_enable_irq(int irq) {
    writel(1 << (irq % 32),
           gicd_base + GICD_ISENABLER + (irq / 32) * 4);
}
```

## 3.4 ARMv8计时器系统寄存器（参考 arm_arch_timer.h）

```
系统中有4个计时器，每CPU各自拥有：

EL1 Physical Timer:
  CNTP_CTL_EL0  - 控制寄存器（ENABLE/IMASK/ISTATUS）
  CNTP_TVAL_EL0 - TimerValue（倒计时值）
  CNTP_CVAL_EL0 - CompareValue（绝对比较值）

EL1 Virtual Timer（我们主要使用这个）:
  CNTV_CTL_EL0
  CNTV_TVAL_EL0
  CNTV_CVAL_EL0

计数器（只读，全局）:
  CNTPCT_EL0    - 物理计数值
  CNTVCT_EL0    - 虚拟计数值
  CNTFRQ_EL0    - 计数频率（QEMU通常为62.5MHz = 62500000）

PPI中断号：
  Physical Timer = PPI #30 (IRQ 30)
  Virtual Timer  = PPI #27 (IRQ 27)
```

## 3.5 arch timer实现（参考 arm_arch_timer.c）

```c
#define ARCH_TIMER_CTL_ENABLE   (1 << 0)
#define ARCH_TIMER_CTL_IMASK    (1 << 1)  /* 1=屏蔽中断 */
#define ARCH_TIMER_CTL_ISTATUS  (1 << 2)  /* 1=定时器已触发 */

uint64_t arch_timer_get_cntfrq(void) {
    uint64_t val;
    asm volatile("mrs %0, cntfrq_el0" : "=r"(val));
    return val;
}

/* 设置定时器：在N纳秒后触发中断 */
void arch_timer_set_next_event_ns(uint64_t ns) {
    uint64_t freq = arch_timer_get_cntfrq();  /* 62500000 */
    uint64_t ticks = (ns * freq) / 1000000000ULL;

    asm volatile("msr cntv_tval_el0, %0" :: "r"(ticks));
    asm volatile("msr cntv_ctl_el0, %0" :: "r"((uint64_t)ARCH_TIMER_CTL_ENABLE));
    asm volatile("isb");
}

/* 计时器中断处理 */
void arch_timer_handler(void) {
    /* 屏蔽计时器（避免重复触发）*/
    asm volatile("msr cntv_ctl_el0, %0" ::
                 "r"((uint64_t)(ARCH_TIMER_CTL_ENABLE | ARCH_TIMER_CTL_IMASK)));

    /* 触发调度（tick）*/
    scheduler_tick();

    /* 重新设置下次中断（HZ=100, 10ms）*/
    arch_timer_set_next_event_ns(10000000ULL);
}
```

## 3.6 验证方法

```c
void test_gic_timer(void) {
    static volatile int tick_count = 0;

    /* 注册计时器中断处理函数（Virtual Timer PPI #27）*/
    irq_handlers[27] = arch_timer_handler;

    /* 启动计时器：10ms后触发 */
    arch_timer_set_next_event_ns(10000000ULL);

    /* 等待10个tick（约100ms）*/
    while (tick_count < 10);

    printk("GIC v3 + arch timer: OK, %d ticks\n", tick_count);
}
```

## 3.7 本阶段产出文件

```
arm64os/
├── drivers/
│   ├── irqchip/
│   │   └── gic-v3.c              ← GIC v3初始化与中断处理（核心）
│   └── timer/
│       └── arm_arch_timer.c      ← ARMv8通用计时器（核心）
├── kernel/irq/
│   ├── irqdesc.c                 ← IRQ描述符管理
│   └── handle.c                  ← 中断处理框架
└── arch/arm64/kernel/entry.S     ← 更新：添加IRQ处理路径
```
