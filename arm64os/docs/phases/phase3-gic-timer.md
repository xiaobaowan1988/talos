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
- **Distributor（GICD）**：全局，管理所有中断的使能/优先级/路由，QEMU virt MMIO地址：`0x08000000`
- **Redistributor（GICR）**：每CPU一个，管理PPI和SGI，QEMU virt CPU0地址：`0x080A0000`

每个Redistributor由两个64KB帧组成：
- **RD帧**（`GICR_RD_BASE`，偏移 `+0x00000`）：控制、唤醒等
- **SGI帧**（`GICR_SGI_BASE = GICR_RD_BASE + 0x10000`）：SGI/PPI使能、优先级

多CPU时每CPU stride为 `0x20000`（128KB）：
- CPU0 RD帧：`0x080A0000`，SGI帧：`0x080B0000`
- CPU1 RD帧：`0x080C0000`，SGI帧：`0x080D0000`

中断分类：
```
SGI  (0-15)   : Software Generated Interrupts，核间通信
PPI  (16-31)  : Per-CPU Private Interrupts，如arch timer (PPI #27, #30)
SPI  (32-1019): Shared Peripheral Interrupts，设备中断
```

## 3.2 GIC v3寄存器映射（参考 arm-gic-v3.h）

```c
/* ---- GICD寄存器偏移（相对 GICD_BASE = 0x08000000）---- */
#define GICD_CTLR           0x0000  /* 分发器控制 */
#define GICD_TYPER          0x0004  /* 类型寄存器（中断数量等）*/
#define GICD_ISENABLER      0x0100  /* SPI中断使能设置（每位一个中断）*/
#define GICD_ICENABLER      0x0180  /* SPI中断使能清除 */
#define GICD_ISPENDR        0x0200  /* pending设置 */
#define GICD_IPRIORITYR     0x0400  /* 中断优先级（每字节一个中断）*/
#define GICD_ICFGR          0x0C00  /* 边沿/电平触发配置 */
#define GICD_IROUTER        0x6000  /* SPI路由寄存器（v3新增，64位/中断）*/

/* GICD_CTLR bit定义 */
#define GICD_CTLR_ENABLE_G0     (1U << 0)   /* Group 0 使能 */
#define GICD_CTLR_ENABLE_G1NS   (1U << 1)   /* Group 1 NS 使能 */
#define GICD_CTLR_ARE_NS        (1U << 4)   /* Affinity Routing Enable (NS) */
#define GICD_CTLR_RWP           (1U << 31)  /* Read-Write-Pending（轮询直到清零）*/

/* ---- GICR寄存器偏移 ---- */
/*
 * RD帧（相对 GICR_RD_BASE）：
 *   GICR_RD_BASE = 0x080A0000（CPU0）
 */
#define GICR_TYPER          0x0008  /* 类型寄存器 */
#define GICR_WAKER          0x0014  /* 唤醒寄存器（ProcessorSleep/ChildrenAsleep）*/

/*
 * SGI帧（相对 GICR_SGI_BASE = GICR_RD_BASE + 0x10000）：
 *   GICR_SGI_BASE = 0x080B0000（CPU0）
 */
#define GICR_SGI_OFFSET     0x10000             /* SGI帧相对RD帧的偏移 */
#define GICR_ISENABLER0     0x0100  /* SGI/PPI使能（bit N对应IRQ N，N∈[0,31]）*/
#define GICR_ICENABLER0     0x0180  /* SGI/PPI使能清除 */
#define GICR_IPRIORITYR0    0x0400  /* SGI/PPI优先级（每字节一个中断）*/

/* System registers（ICC_*）直接通过MSR/MRS访问 */
// ICC_SRE_EL1:    使能系统寄存器接口（SRE=1, DFB=1, DIB=1，写7）
// ICC_PMR_EL1:    优先级掩码（0xFF = 允许所有中断）
// ICC_BPR1_EL1:   Binary Point Register（写0）
// ICC_IGRPEN1_EL1: Group 1中断全局使能（写1）
// ICC_IAR1_EL1:   Interrupt Acknowledge（读取中断号并标记Active）
// ICC_EOIR1_EL1:  End of Interrupt（写中断号标记处理完成）
```

## 3.3 GIC v3初始化流程

```c
/* 参考 drivers/irqchip/irq-gic-v3.c : gic_init_bases() */

/* QEMU virt machine固定地址 */
#define GICD_BASE   ((volatile void *)0x08000000UL)
#define GICR_BASE   ((volatile void *)0x080A0000UL)  /* CPU0 RD帧 */

void gicv3_init(void) {
    volatile void *gicd = GICD_BASE;
    volatile void *gicr = GICR_BASE;
    volatile void *gicr_sgi = (volatile void *)((unsigned long)gicr + GICR_SGI_OFFSET);

    /* ---- Step 1: Distributor初始化 ---- */

    /* 1.1 禁用Distributor，等待RWP清零 */
    writel(0, gicd + GICD_CTLR);
    while (readl(gicd + GICD_CTLR) & GICD_CTLR_RWP)
        ;

    /* 1.2 使能Affinity Routing（GICv3特性）+ Group 1 NS */
    writel(GICD_CTLR_ARE_NS | GICD_CTLR_ENABLE_G1NS, gicd + GICD_CTLR);
    while (readl(gicd + GICD_CTLR) & GICD_CTLR_RWP)
        ;

    /* 1.3 将所有SPI路由到CPU0（Affinity: 0.0.0.0）*/
    for (int i = 32; i < 1020; i++)
        writeq(0, gicd + GICD_IROUTER + i * 8);

    /* 1.4 将所有SPI设置为低优先级（0xA0），禁用 */
    for (int i = 32 / 4; i < 1020 / 4; i++)
        writel(0xA0A0A0A0U, gicd + GICD_IPRIORITYR + i * 4);
    for (int i = 32 / 32; i < 1020 / 32; i++)
        writel(0xFFFFFFFFU, gicd + GICD_ICENABLER + i * 4);

    /* ---- Step 2: Redistributor初始化（CPU0）---- */

    /* 2.1 将CPU从Sleep状态唤醒（清除 GICR_WAKER.ProcessorSleep, bit 1）*/
    uint32_t waker = readl(gicr + GICR_WAKER);
    waker &= ~(1U << 1);
    writel(waker, gicr + GICR_WAKER);
    /* 等待 ChildrenAsleep（bit 2）清零 */
    while (readl(gicr + GICR_WAKER) & (1U << 2))
        ;

    /* 2.2 将所有SGI/PPI（IRQ 0-31）设置为低优先级（0xA0），初始禁用 */
    for (int i = 0; i < 32 / 4; i++)
        writel(0xA0A0A0A0U, gicr_sgi + GICR_IPRIORITYR0 + i * 4);
    writel(0xFFFFFFFFU, gicr_sgi + GICR_ICENABLER0);

    /* ---- Step 3: CPU接口初始化（通过系统寄存器）---- */

    /* 3.1 使能系统寄存器接口（ICC_SRE_EL1: SRE=1, DFB=1, DIB=1）*/
    asm volatile("msr ICC_SRE_EL1, %0" :: "r"(7UL));
    asm volatile("isb");

    /* 3.2 设置优先级掩码（允许所有优先级中断通过）*/
    asm volatile("msr ICC_PMR_EL1, %0" :: "r"(0xFFUL));

    /* 3.3 设置Binary Point（不分组）*/
    asm volatile("msr ICC_BPR1_EL1, %0" :: "r"(0UL));

    /* 3.4 使能Group 1中断 */
    asm volatile("msr ICC_IGRPEN1_EL1, %0" :: "r"(1UL));
    asm volatile("isb");
}

/* 使能某个SPI中断（IRQ >= 32）*/
void gicv3_enable_irq(unsigned int irq) {
    volatile void *gicd = GICD_BASE;
    writel(1U << (irq % 32), gicd + GICD_ISENABLER + (irq / 32) * 4);
}

/* 使能某个PPI/SGI中断（IRQ < 32，在Redistributor SGI帧操作）*/
void gicv3_enable_ppi(unsigned int irq) {
    volatile void *gicr_sgi =
        (volatile void *)(0x080A0000UL + GICR_SGI_OFFSET);  /* CPU0 SGI帧 */
    writel(1U << irq, gicr_sgi + GICR_ISENABLER0);
}
```

## 3.4 中断处理（kernel/irq/handle.c）

```c
/* IRQ处理函数表（irqdesc.c中定义）*/
extern irq_handler_t irq_handlers[NR_IRQS];

/*
 * handle_irq - 中断分发入口
 *
 * 由 entry.S 中 el1h_irq / el0_irq 调用，传入 pt_regs 指针。
 * 通过 ICC_IAR1_EL1 读取中断号，分发到注册的处理函数，最后 EOI。
 */
void handle_irq(struct pt_regs *regs) {
    uint64_t irqnr;

    /* 读取中断号（同时标记该中断为Active状态）*/
    asm volatile("mrs %0, ICC_IAR1_EL1" : "=r"(irqnr));
    irqnr &= 0x3FF;  /* 取低10位（有效中断号范围 0-1019）*/

    /* 1020-1023 是特殊值（spurious/no interrupt），忽略 */
    if (irqnr < 1020) {
        if (irq_handlers[irqnr])
            irq_handlers[irqnr]();
    }

    /* End of Interrupt：通知GIC中断处理完成 */
    asm volatile("msr ICC_EOIR1_EL1, %0" :: "r"(irqnr));
    asm volatile("isb");
}
```

## 3.5 ARMv8计时器系统寄存器（参考 arm_arch_timer.h）

```
系统中有4个计时器，每CPU各自拥有：

EL1 Physical Timer:
  CNTP_CTL_EL0  - 控制寄存器（ENABLE/IMASK/ISTATUS）
  CNTP_TVAL_EL0 - TimerValue（倒计时值）
  CNTP_CVAL_EL0 - CompareValue（绝对比较值）

EL1 Virtual Timer（本阶段使用）:
  CNTV_CTL_EL0  - 控制
  CNTV_TVAL_EL0 - TimerValue（倒计时，写入后开始倒数）
  CNTV_CVAL_EL0 - CompareValue（绝对比较值）

计数器（只读，全局）:
  CNTPCT_EL0    - 物理计数值
  CNTVCT_EL0    - 虚拟计数值
  CNTFRQ_EL0    - 计数频率（QEMU通常为62.5MHz = 62500000）

PPI中断号：
  Physical Timer = PPI #30 (IRQ 30)
  Virtual Timer  = PPI #27 (IRQ 27)  ← 本阶段使用
```

## 3.6 arch timer实现（参考 arm_arch_timer.c）

```c
#define ARCH_TIMER_CTL_ENABLE   (1U << 0)
#define ARCH_TIMER_CTL_IMASK    (1U << 1)  /* 1=屏蔽中断输出（计时器仍运行）*/
#define ARCH_TIMER_CTL_ISTATUS  (1U << 2)  /* 1=定时器已触发（只读）*/

/* 全局tick计数器（volatile确保编译器每次从内存读取）*/
volatile int arch_timer_tick_count = 0;

uint64_t arch_timer_get_cntfrq(void) {
    uint64_t val;
    asm volatile("mrs %0, cntfrq_el0" : "=r"(val));
    return val;
}

/* 设置Virtual Timer：在N纳秒后触发中断 */
void arch_timer_set_next_event_ns(uint64_t ns) {
    uint64_t freq = arch_timer_get_cntfrq();  /* QEMU: 62500000 Hz */
    uint64_t ticks = (ns * freq) / 1000000000ULL;

    /* 写CNTV_TVAL_EL0：从当前计数器值倒数 ticks 后触发中断 */
    asm volatile("msr cntv_tval_el0, %0" :: "r"(ticks));
    /* 使能计时器，清除IMASK */
    asm volatile("msr cntv_ctl_el0, %0" ::
                 "r"((uint64_t)ARCH_TIMER_CTL_ENABLE));
    asm volatile("isb");
}

/* Virtual Timer PPI #27 中断处理函数 */
void arch_timer_handler(void) {
    /* 先屏蔽计时器中断（避免在重新装载前重复触发）*/
    asm volatile("msr cntv_ctl_el0, %0" ::
                 "r"((uint64_t)(ARCH_TIMER_CTL_ENABLE | ARCH_TIMER_CTL_IMASK)));

    /* 递增tick计数（由start_kernel轮询）*/
    arch_timer_tick_count++;

    /* 触发调度器tick（Phase 4实现，此处留空）*/
    /* scheduler_tick(); */

    /* 重新设置下次中断（HZ=100，10ms）*/
    arch_timer_set_next_event_ns(10000000ULL);
}

/* 初始化arch timer：注册处理函数，使能PPI，启动第一次计时 */
void arch_timer_init(void) {
    /* 注册Virtual Timer中断处理函数（PPI #27）*/
    request_irq(27, arch_timer_handler);

    /* 在Redistributor中使能PPI #27 */
    gicv3_enable_ppi(27);

    /* 启动计时器，10ms后第一次触发 */
    arch_timer_set_next_event_ns(10000000ULL);
}
```

## 3.7 验证方法

```c
/* 在start_kernel中，GIC + timer初始化完成后调用 */
void test_gic_timer(void) {
    extern volatile int arch_timer_tick_count;

    /* 等待10个tick（约100ms） */
    while (arch_timer_tick_count < 10)
        ;

    boot_printk("[TEST] GIC v3 + arch timer: PASS, ticks=10\n");
}
```

Phase 3 验证序列（start_kernel中）：
1. `gicv3_init()` — 初始化GIC v3
2. `arch_timer_init()` — 注册处理函数 + 使能PPI #27 + 启动计时
3. `asm("msr daifclr, #2")` — 开放IRQ（清除DAIF I位）
4. 等待 `arch_timer_tick_count >= 10`
5. 打印 `"Phase 3 complete"`

## 3.8 本阶段产出文件

```
arm64os/
├── include/linux/
│   └── irq.h                         ← IRQ子系统头文件（新增）
├── drivers/
│   ├── irqchip/
│   │   └── gic-v3.c                  ← GIC v3初始化与中断处理（新增）
│   └── timer/
│       └── arm_arch_timer.c          ← ARMv8通用计时器（新增）
├── kernel/irq/
│   ├── irqdesc.c                     ← IRQ描述符管理（新增）
│   └── handle.c                      ← 中断处理框架（新增，替代main.c的stub）
├── kernel/
│   └── main.c                        ← 更新：Phase 3初始化序列
├── arch/arm64/kernel/
│   └── entry.S                       ← 已有IRQ路径（el1h_irq→handle_irq），无需修改
└── Makefile                          ← 更新：添加Phase 3目标文件
```

注：`arch/arm64/kernel/entry.S` 在Phase 1/2中已实现 `el1h_irq` → `handle_irq` 调用路径，Phase 3无需修改entry.S。
