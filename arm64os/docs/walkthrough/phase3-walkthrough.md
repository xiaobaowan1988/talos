# Phase 3 Walkthrough: GIC v3 中断控制器 + ARM 通用定时器

> **目标**：让内核响应硬件中断，建立时钟心跳。
> **最终效果**：每 10ms 收到一次定时器中断，验证收到 10 个 tick。

---

## 3.1 为什么需要中断？

Phase 1-2 的内核是"单线程"的：CPU 执行完一条指令才执行下一条，没有任何异步事件。但操作系统必须：

- **响应硬件事件** — 磁盘读完数据、网卡收到包、键盘按键
- **实现时间片** — 定时器中断驱动进程切换（Phase 4）
- **防止死循环** — 某个进程卡死不会影响整个系统

ARM64 的中断由 **GIC**（Generic Interrupt Controller）管理。

---

## 3.2 GIC v3 架构

```
  ┌─────────┐    IRQ 线     ┌──────────────────┐
  │ 设备 A  │───────────────┤                  │
  │ 设备 B  │───────────────┤   Distributor    │  全局唯一
  │ 设备 C  │───────────────┤   (GICD)         │  路由 SPI
  └─────────┘               └────────┬─────────┘
                                     │
                            ┌────────┴─────────┐
                            │                  │
                      ┌─────┴──────┐    ┌──────┴─────┐
                      │Redistributor│   │Redistributor│  每 CPU 一个
                      │  CPU 0     │    │  CPU 1     │  管理 SGI/PPI
                      └─────┬──────┘    └──────┬─────┘
                            │                  │
                      ┌─────┴──────┐    ┌──────┴─────┐
                      │ CPU 接口   │    │ CPU 接口   │  系统寄存器
                      │(ICC_*_EL1) │    │(ICC_*_EL1) │  读/写/确认
                      └────────────┘    └────────────┘
```

### 中断类型

| 类型 | INTID | 说明 |
|------|-------|------|
| SGI (Software Generated) | 0-15 | 核间中断 |
| PPI (Private Peripheral) | 16-31 | 每 CPU 私有（**定时器 = PPI #27**） |
| SPI (Shared Peripheral) | 32-1019 | 共享设备中断（VirtIO 等） |

---

## 3.3 GIC Distributor 初始化

Distributor 是全局的，管理 SPI（共享中断）的路由。MMIO 基址 `0x08000000`。

```c
#define GICD_BASE           0x08000000UL

void gicv3_dist_init(void)
{
    /* Step 1: 关闭 Distributor */
    writel(0, GICD_BASE + GICD_CTLR);

    /* 等待 RWP（Register Write Pending）清零 */
    while (readl(GICD_BASE + GICD_CTLR) & GICD_CTLR_RWP)
        ;

    /* Step 2: 所有 SPI 设为 Group 1 Non-Secure */
    for (i = 1; i < (1020 / 32); i++)
        writel(0xFFFFFFFF, GICD_BASE + GICD_IGROUPR + i * 4);

    /* Step 3: 所有 SPI 默认优先级 0xA0 */
    for (i = 8; i < (1020 / 4); i++)
        writel(0xA0A0A0A0, GICD_BASE + GICD_IPRIORITYR + i * 4);

    /* Step 4: 所有 SPI 禁用 */
    for (i = 1; i < (1020 / 32); i++)
        writel(0xFFFFFFFF, GICD_BASE + GICD_ICENABLER + i * 4);

    /* Step 5: 所有 SPI 路由到 CPU 0 */
    for (i = 32; i < 1020; i++)
        writeq(0, GICD_BASE + GICD_IROUTER + (unsigned long)i * 8);

    /* Step 6: 开启 Distributor（启用 Affinity Routing + Group 1 NS） */
    writel(GICD_CTLR_ARE_NS | GICD_CTLR_ENABLE_G1NS,
           GICD_BASE + GICD_CTLR);
}
```

---

## 3.4 GIC Redistributor 初始化

Redistributor 管理每个 CPU 的 SGI/PPI。基址 `0x080A0000`。

```c
#define GICR_BASE           0x080A0000UL

void gicv3_redist_init(void)
{
    /* Step 1: 唤醒 Redistributor */
    val = readl(GICR_BASE + GICR_WAKER);
    val &= ~GICR_WAKER_ProcessorSleep;  /* 清除睡眠标志 */
    writel(val, GICR_BASE + GICR_WAKER);

    /* 等待 ChildrenAsleep 清零 */
    while (readl(GICR_BASE + GICR_WAKER) & GICR_WAKER_ChildrenAsleep)
        ;

    /* Step 2: SGI/PPI (0-31) 设为 Group 1 NS */
    writel(0xFFFFFFFF, GICR_SGI_BASE + GICR_IGROUPR0);

    /* Step 3: 默认优先级 0xA0 */
    for (i = 0; i < 8; i++)
        writel(0xA0A0A0A0, GICR_SGI_BASE + GICR_IPRIORITYR + i * 4);

    /* Step 4: 禁用所有 SGI/PPI */
    writel(0xFFFFFFFF, GICR_SGI_BASE + GICR_ICENABLER0);
}
```

---

## 3.5 CPU Interface 初始化

CPU Interface 使用 **系统寄存器**（不是 MMIO），通过 `msr`/`mrs` 指令访问。

```c
void gicv3_cpu_init(void)
{
    /* 启用系统寄存器访问 */
    write_sysreg(7, ICC_SRE_EL1);  /* SRE=1, DFB=1, DIB=1 */

    /* 设置优先级掩码：接受所有中断 */
    write_sysreg(0xFF, ICC_PMR_EL1);

    /* 无优先级分组 */
    write_sysreg(0, ICC_BPR1_EL1);

    /* 启用 Group 1 中断 */
    write_sysreg(1, ICC_IGRPEN1_EL1);

    isb();  /* 确保设置立即生效 */
}
```

**中断优先级**：数值越小优先级越高。`ICC_PMR_EL1 = 0xFF` 表示接受所有优先级的中断。

---

## 3.6 IRQ 处理流程

当硬件中断到达时，CPU 自动跳到异常向量表的 IRQ 入口：

```
  硬件中断信号
      │
      ▼
  ┌─────────────┐
  │ el1h_irq    │  entry.S 异常向量
  │ kernel_entry│  保存寄存器
  └─────┬───────┘
        │ bl handle_irq
        ▼
  ┌─────────────────────────────────────┐
  │ handle_irq(pt_regs *regs)          │
  │                                     │
  │  1. irqnr = read ICC_IAR1_EL1      │  读取中断号（同时标记 Active）
  │  2. if (irq_handlers[irqnr])       │  查找注册的处理函数
  │       irq_handlers[irqnr]()        │  调用处理函数
  │  3. write ICC_EOIR1_EL1 = irqnr    │  标记中断处理完成
  │                                     │
  └─────────────────────────────────────┘
        │
        ▼
  kernel_exit → eret                     恢复现场，返回被中断的代码
```

### handle_irq 实现

```c
void handle_irq(struct pt_regs *regs)
{
    unsigned int irqnr;

    /* 读取 IAR（Interrupt Acknowledge Register）*/
    __asm__ volatile("mrs %0, " ICC_IAR1_EL1 : "=r"(irqnr));

    if (irqnr >= 1020)   /* 特殊值：无有效中断 */
        return;

    /* 分发到注册的处理函数 */
    if (irqnr < NR_IRQS && irq_handlers[irqnr])
        irq_handlers[irqnr]();

    /* 标记中断处理完成 */
    __asm__ volatile("msr " ICC_EOIR1_EL1 ", %0" :: "r"(irqnr));
}
```

### request_irq — 注册中断处理函数

```c
irq_handler_t irq_handlers[NR_IRQS];  /* 全局处理函数数组 */

int request_irq(unsigned int irq, irq_handler_t handler)
{
    if (irq >= NR_IRQS)
        return -1;
    irq_handlers[irq] = handler;
    return 0;
}
```

---

## 3.7 ARM 通用定时器

ARM64 CPU 内置定时器，频率由 `CNTFRQ_EL0` 提供（QEMU 默认 62.5 MHz）。

### 关键寄存器

| 寄存器 | 用途 |
|--------|------|
| `CNTFRQ_EL0` | 定时器频率（Hz） |
| `CNTVCT_EL0` | 当前计数值（64 位递增计数器） |
| `CNTV_TVAL_EL0` | 倒计时值（写入后开始倒数） |
| `CNTV_CTL_EL0` | 控制寄存器（ENABLE / IMASK） |

### 设置下一次定时器事件

```c
static void arch_timer_set_next_event_ns(unsigned long ns)
{
    unsigned long cntfrq;
    unsigned long ticks;

    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(cntfrq));
    ticks = (ns * cntfrq) / 1000000000UL;  /* ns → tick */

    __asm__ volatile("msr cntv_tval_el0, %0" :: "r"(ticks));

    /* 使能定时器，清除中断屏蔽 */
    __asm__ volatile("msr cntv_ctl_el0, %0" :: "r"(1UL));
}
```

### 定时器中断处理

```c
#define TIMER_INTERVAL_NS  10000000UL  /* 10ms */

volatile int arch_timer_tick_count = 0;

static void arch_timer_handler(void)
{
    /* 先屏蔽中断（防止重入） */
    __asm__ volatile("msr cntv_ctl_el0, %0" :: "r"(3UL));
    /* bit 0=ENABLE, bit 1=IMASK → 使能但屏蔽 */

    arch_timer_tick_count++;

    /* Phase 4 加入：调用调度器 tick */
    scheduler_tick();

    /* 设置下一次 10ms 后触发 */
    arch_timer_set_next_event_ns(TIMER_INTERVAL_NS);
}
```

### 初始化

```c
void arch_timer_init(void)
{
    /* 注册 PPI #27 的中断处理函数 */
    request_irq(27, arch_timer_handler);

    /* 使能 PPI #27 */
    gicv3_enable_ppi(27);

    /* 启动第一个 10ms 定时 */
    arch_timer_set_next_event_ns(TIMER_INTERVAL_NS);
}
```

---

## 3.8 使能 IRQ — 打开中断大门

```c
/* 在 start_kernel() 中 */
boot_printk("[BOOT] Enabling IRQ (daifclr #2)...\n");
__asm__ volatile("msr daifclr, #2" ::: "memory");
```

从此刻起，中断开始流动：

```
  Timer 倒计时 → 0
      │
      ▼
  PPI #27 → GIC Redistributor → CPU Interface
      │
      ▼
  CPU 检测 IRQ（DAIF.I = 0，未屏蔽）
      │
      ▼
  跳转到 VBAR_EL1 + 0x280 (el1h_irq)
      │
      ▼
  handle_irq() → ICC_IAR1 = 27
      │
      ▼
  arch_timer_handler()
      │
      ├─ tick_count++
      ├─ 设置下一个 10ms
      └─ ICC_EOIR1 = 27
```

---

## 3.9 验证代码

```c
/* 等待 10 个 timer tick (约 100ms) */
boot_printk("[BOOT] Waiting for 10 timer ticks...\n");
while (arch_timer_tick_count < 10)
    ;

boot_printk("[BOOT] Timer ticks: OK (received >= 10)\n");
boot_printk("[BOOT] Phase 3 complete\n");
```

`arch_timer_tick_count` 被中断处理函数递增。主循环只是轮询等待，每次中断都会打断 `while` 循环，执行 handler，然后回到循环。

---

## 3.10 Phase 3 核心概念总结

| 概念 | 说明 |
|------|------|
| **GIC v3** | ARM 标准中断控制器，三层架构 |
| **Distributor** | 全局 SPI 路由（GICD，MMIO 0x08000000） |
| **Redistributor** | 每 CPU 的 SGI/PPI 管理（GICR，MMIO 0x080A0000） |
| **CPU Interface** | 系统寄存器 ICC_*_EL1，读/写/确认中断 |
| **IAR/EOIR** | 读 IAR 获取中断号并标记 Active；写 EOIR 标记完成 |
| **PPI #27** | ARM Virtual Timer 的私有中断 |
| **CNTV_TVAL_EL0** | 倒计时寄存器，到 0 触发中断 |
| **10ms tick** | 操作系统心跳，驱动调度器（Phase 4） |

**Phase 3 奠定的基础**：有了中断和定时器，Phase 4 的调度器就有了"时钟"来驱动进程切换。
