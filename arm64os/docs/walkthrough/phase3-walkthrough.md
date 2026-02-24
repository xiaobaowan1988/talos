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

---

## 3.11 完整源码清单

> Phase 3 在 Phase 2 基础上新增 5 个源文件和 1 个头文件，修改 `arch/arm64/kernel/entry.S`、`kernel/main.c` 和 `Makefile`。

### 新增目录结构

```
arm64os/
├── arch/arm64/
│   ├── include/asm/
│   │   ├── memory.h      (Phase 2, 不变)
│   │   ├── pgtable.h     (Phase 2, 不变)
│   │   └── sysreg.h      (Phase 2, 不变)
│   ├── kernel/
│   │   ├── head.S         (Phase 1, 不变)
│   │   └── entry.S        ← 修改（el1h_irq / el0_irq 调用 handle_irq）
│   └── mm/
│       ├── mmu.c          (Phase 2, 不变)
│       ├── proc.S         (Phase 2, 不变)
│       └── tlb.S          (Phase 2, 不变)
├── drivers/
│   ├── irqchip/
│   │   └── gic-v3.c       ← 新增
│   └── timer/
│       └── arm_arch_timer.c ← 新增
├── include/linux/
│   ├── types.h            (Phase 1, 不变)
│   ├── list.h             (Phase 2, 不变)
│   ├── io.h               (Phase 2, 不变)
│   └── irq.h              ← 新增
├── kernel/
│   ├── irq/
│   │   ├── handle.c       ← 新增
│   │   └── irqdesc.c      ← 新增
│   ├── main.c             ← 修改
│   └── printk.c           (Phase 1, 不变)
├── mm/
│   ├── memblock.c         (Phase 2, 不变)
│   └── page_alloc.c       (Phase 2, 不变)
├── scripts/linker.ld      (Phase 1, 不变)
└── Makefile               ← 修改
```

### 新增文件 1: `include/linux/irq.h`

```c
/* SPDX-License-Identifier: GPL-2.0 */
/*
 * arm64os/include/linux/irq.h
 *
 * IRQ 子系统头文件
 *
 * 参考：include/linux/irq.h, include/linux/interrupt.h
 *
 * Phase 3 简化实现：
 *   - 固定大小的 irq_handler_t 函数指针数组（1020项）
 *   - request_irq() 注册中断处理函数
 *   - handle_irq() 由 entry.S 调用，通过 GIC IAR 分发中断
 */

#ifndef __LINUX_IRQ_H
#define __LINUX_IRQ_H

/*
 * NR_IRQS：GIC v3 支持的最大中断数
 * SPI 上限为 IRQ 1019，保留 1020-1023 为特殊值（spurious 等）
 */
#define NR_IRQS     1020

/*
 * irq_handler_t：中断处理函数类型
 *
 * Phase 3 使用无参数无返回值的简单函数指针。
 * Linux 内核实际使用 irqreturn_t (*handler)(int, void *)。
 */
typedef void (*irq_handler_t)(void);

/*
 * irq_handlers[]：IRQ 处理函数表
 *
 * 定义在 kernel/irq/irqdesc.c，按 IRQ 号索引。
 * 未注册的项为 NULL。
 *
 * IRQ 布局（GIC v3）：
 *   [0-15]  : SGI（核间中断，Phase 3 暂不使用）
 *   [16-31] : PPI（每CPU私有中断，#27 = Virtual Timer）
 *   [32-1019]: SPI（共享外设中断，Phase 6 VirtIO 使用）
 */
extern irq_handler_t irq_handlers[NR_IRQS];

/*
 * request_irq - 注册中断处理函数
 *
 * @irq:     中断号（0-1019）
 * @handler: 处理函数指针（NULL 表示注销）
 *
 * 参考：include/linux/interrupt.h: request_irq()
 */
void request_irq(unsigned int irq, irq_handler_t handler);

#endif /* __LINUX_IRQ_H */
```

### 新增文件 2: `kernel/irq/irqdesc.c`

```c
/* SPDX-License-Identifier: GPL-2.0 */
/*
 * arm64os/kernel/irq/irqdesc.c
 *
 * IRQ 描述符管理
 *
 * 参考：kernel/irq/irqdesc.c, kernel/irq/manage.c
 *
 * Phase 3 简化实现：
 *   - irq_handlers[]：IRQ 处理函数指针数组，按 IRQ 号索引
 *   - request_irq()：注册/注销处理函数
 *
 * Linux 内核的 struct irq_desc 包含锁、统计计数、设备信息等字段。
 * 这里仅保留最核心的函数指针，足以驱动 GIC v3 + arch timer。
 */

#include <linux/types.h>
#include <linux/irq.h>

/*
 * irq_handlers[] - 中断处理函数表
 *
 * 全局数组，初始化为全 NULL。
 * 由 request_irq() 填充，由 handle_irq() 读取分发。
 *
 * 参考：Linux 内核通过 irq_desc[NR_IRQS] 数组管理每个 IRQ 的描述符；
 *       此处将其简化为单一函数指针数组。
 */
irq_handler_t irq_handlers[NR_IRQS];

/*
 * request_irq - 注册中断处理函数
 *
 * @irq:     中断号（0 .. NR_IRQS-1）
 * @handler: 处理函数（NULL 表示注销）
 *
 * 参考：kernel/irq/manage.c: __setup_irq()
 */
void request_irq(unsigned int irq, irq_handler_t handler)
{
    if (irq >= NR_IRQS)
        return;

    irq_handlers[irq] = handler;
}
```

### 新增文件 3: `kernel/irq/handle.c`

```c
/* SPDX-License-Identifier: GPL-2.0 */
/*
 * arm64os/kernel/irq/handle.c
 *
 * 中断处理框架
 *
 * 参考：kernel/irq/handle.c, arch/arm64/kernel/irq.c
 *
 * 此文件实现 handle_irq()，由 entry.S 中的 el1h_irq / el0_irq 调用。
 *
 * 处理流程：
 *   1. 通过 ICC_IAR1_EL1 读取中断号（Interrupt Acknowledge Register）
 *      — 读取此寄存器同时将中断标记为 Active 状态
 *   2. 根据中断号分发到 irq_handlers[] 中注册的处理函数
 *   3. 通过 ICC_EOIR1_EL1 写入 End-of-Interrupt（将中断标记为 Inactive）
 *
 * 中断号特殊值（GIC 规范）：
 *   1020 = spurious interrupt（无中断，硬件返回的哨兵值）
 *   1021-1023 = reserved
 */

#include <linux/types.h>
#include <linux/irq.h>

/*
 * pt_regs 前向声明（定义在 kernel/main.c）
 * entry.S 调用时通过 x0 传入 sp（即 pt_regs 指针）。
 */
struct pt_regs;

/*
 * handle_irq - 中断分发入口
 *
 * @regs: 指向栈上 pt_regs 结构体的指针（由 kernel_entry 宏构建）
 *
 * 参考：arch/arm64/kernel/irq.c: handle_arch_irq()
 *       kernel/irq/handle.c: handle_irq_event_percpu()
 */
void handle_irq(struct pt_regs *regs)
{
    u64 irqnr;

    (void)regs;

    /*
     * 读取 Interrupt Acknowledge Register（Group 1）。
     * 此操作同时将该中断状态从 Pending 变为 Active。
     * 低 10 位为中断号，高位保留。
     *
     * 参考：ARM GIC Architecture Specification,
     *       Section 4.8.12: ICC_IAR1_EL1
     */
    __asm__ volatile("mrs %0, ICC_IAR1_EL1" : "=r"(irqnr));
    irqnr &= 0x3FFU;  /* 取低10位：有效中断号范围 0-1019 */

    /*
     * 分发中断。
     * IRQ 1020-1023 为特殊值（spurious/no interrupt），跳过处理但仍需 EOI。
     */
    if (irqnr < NR_IRQS && irq_handlers[irqnr])
        irq_handlers[irqnr]();

    /*
     * End of Interrupt：通知 GIC CPU 接口已完成对该中断的处理。
     * 将该中断状态从 Active 变回 Inactive（或 Pending，若中断再次触发）。
     *
     * 参考：ARM GIC Architecture Specification,
     *       Section 4.8.14: ICC_EOIR1_EL1
     */
    __asm__ volatile("msr ICC_EOIR1_EL1, %0" :: "r"(irqnr));
    __asm__ volatile("isb");
}
```

### 新增文件 4: `drivers/irqchip/gic-v3.c`

```c
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
```

### 新增文件 5: `drivers/timer/arm_arch_timer.c`

> Phase 3 版本：`arch_timer_handler()` 中调用 `scheduler_tick()`，但 Phase 3 阶段该函数为空操作桩（Phase 4 才实现）。

```c
/* SPDX-License-Identifier: GPL-2.0 */
/*
 * arm64os/drivers/timer/arm_arch_timer.c
 *
 * ARMv8 通用计时器驱动
 *
 * 参考：drivers/clocksource/arm_arch_timer.c
 *       include/clocksource/arm_arch_timer.h
 *       ARMv8-A Architecture Reference Manual, Section D13.8（Generic Timer）
 *
 * ARMv8 每 CPU 提供多个计时器，本阶段使用 EL1 Virtual Timer：
 *   CNTV_CTL_EL0  — 控制寄存器（ENABLE/IMASK/ISTATUS）
 *   CNTV_TVAL_EL0 — TimerValue（写入倒计时 tick 数）
 *   CNTFRQ_EL0    — 计数频率（QEMU virt 默认 62.5 MHz = 62500000 Hz）
 *
 * Virtual Timer PPI 中断号：#27（IRQ 27 in GIC terms）
 *
 * Phase 3 实现：
 *   - arch_timer_init()：注册 PPI #27 处理函数，使能 PPI，启动计时
 *   - arch_timer_handler()：每 10ms 触发一次，递增 arch_timer_tick_count
 *   - arch_timer_set_next_event_ns()：设置下次中断时间（纳秒）
 *   - arch_timer_tick_count：全局 tick 计数，由 start_kernel() 轮询
 *
 * Phase 4 新增：
 *   - arch_timer_handler() 调用 scheduler_tick() + schedule()，驱动 CFS 调度
 */

#include <linux/types.h>
#include <linux/irq.h>
#include <linux/sched.h>

/* ============================================================
 * 计时器控制寄存器位定义（CNTV_CTL_EL0）
 * 参考：ARMv8-A ARM, Section D13.8.3
 * ============================================================ */
#define ARCH_TIMER_CTL_ENABLE   (1U << 0)  /* 1=计时器运行，0=停止 */
#define ARCH_TIMER_CTL_IMASK    (1U << 1)  /* 1=屏蔽中断输出（计时器仍运行）*/
#define ARCH_TIMER_CTL_ISTATUS  (1U << 2)  /* 只读：1=计时器已到期 */

/* 外部依赖：GIC PPI 使能（drivers/irqchip/gic-v3.c）*/
void gicv3_enable_ppi(unsigned int irq);

/*
 * arch_timer_tick_count - 全局 tick 计数器
 *
 * volatile 确保编译器每次从内存读取，而非从寄存器缓存，
 * 因为该变量在中断上下文（arch_timer_handler）和主线程（start_kernel）
 * 之间共享。
 *
 * 参考：Linux 内核 jiffies 全局计数器的类似使用场景。
 */
volatile int arch_timer_tick_count = 0;

/* ============================================================
 * arch_timer_get_cntfrq - 读取计时器频率
 *
 * 返回 CNTFRQ_EL0，QEMU virt 通常为 62500000（62.5 MHz）。
 * 参考：arm_arch_timer.c: arch_timer_get_cntfrq()
 * ============================================================ */
static u64 arch_timer_get_cntfrq(void)
{
    u64 val;
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(val));
    return val;
}

/* ============================================================
 * arch_timer_set_next_event_ns - 设置 Virtual Timer 下次触发时间
 *
 * @ns: 距现在的纳秒数
 *
 * 写入 CNTV_TVAL_EL0：从当前计数器值开始，经过 ticks 个计数单位后触发。
 * 使能计时器并清除 IMASK，允许中断输出。
 *
 * ticks = ns * cntfrq / 1e9
 *
 * 参考：arm_arch_timer.c: arch_timer_set_next_event_virt()
 * ============================================================ */
void arch_timer_set_next_event_ns(u64 ns)
{
    u64 freq = arch_timer_get_cntfrq();
    u64 ticks = (ns * freq) / 1000000000ULL;

    /* 写入倒计时值（写入即生效）*/
    __asm__ volatile("msr cntv_tval_el0, %0" :: "r"(ticks));
    /* 使能计时器，清除 IMASK，允许中断输出 */
    __asm__ volatile("msr cntv_ctl_el0, %0" ::
                     "r"((u64)ARCH_TIMER_CTL_ENABLE));
    __asm__ volatile("isb");
}

/* ============================================================
 * arch_timer_handler - Virtual Timer PPI #27 中断处理函数
 *
 * 由 handle_irq() 通过 irq_handlers[27] 调用。
 *
 * 处理流程：
 *   1. 屏蔽计时器中断（避免在重新装载期间重复触发）
 *   2. 递增全局 tick 计数
 *   3. 重新设置下次中断（HZ=100，10ms）
 *
 * 参考：arm_arch_timer.c: arch_timer_handler_virt()
 *       kernel/time/tick-common.c: tick_handle_periodic()
 * ============================================================ */
static void arch_timer_handler(void)
{
    /*
     * 先屏蔽计时器中断输出（设置 IMASK，保留 ENABLE）。
     * 这防止在重新装载 TVAL 之前计时器再次触发中断。
     */
    __asm__ volatile("msr cntv_ctl_el0, %0" ::
                     "r"((u64)(ARCH_TIMER_CTL_ENABLE | ARCH_TIMER_CTL_IMASK)));

    /* 递增 tick 计数（start_kernel 轮询此值验证计时器工作）*/
    arch_timer_tick_count++;

    /*
     * 触发调度器 tick：更新当前进程 vruntime，判断是否需要抢占。
     *
     * scheduler_tick() 仅更新 vruntime 和设置 TIF_NEED_RESCHED 标志。
     * 实际的上下文切换不在中断上下文中进行，而是：
     *   - 内核线程主动调用 schedule()（协作式）
     *   - 中断返回路径检查标志（Phase 5 完善）
     *
     * Phase 4 使用协作式调度：线程定期调用 schedule() 检查标志。
     *
     * 参考：kernel/time/tick-common.c tick_handle_periodic()
     */
    scheduler_tick();

    /* 重新设置下次中断：10ms（HZ=100）*/
    arch_timer_set_next_event_ns(10000000ULL);
}

/* ============================================================
 * arch_timer_init - 初始化 ARMv8 Virtual Timer
 *
 * 调用时机：start_kernel() 中，gicv3_init() 之后，daifclr 之前。
 *
 * 参考：arm_arch_timer.c: arch_timer_register()
 *       arch/arm64/kernel/time.c: time_init()
 * ============================================================ */
void arch_timer_init(void)
{
    /*
     * 注册 Virtual Timer 中断处理函数（PPI #27）。
     * GIC Virtual Timer → PPI #27 → irq_handlers[27]。
     */
    request_irq(27, arch_timer_handler);

    /*
     * 在 Redistributor 的 SGI 帧中使能 PPI #27。
     * 必须在 GIC 初始化（gicv3_init）之后调用。
     */
    gicv3_enable_ppi(27);

    /*
     * 启动计时器：10ms 后触发第一次中断。
     * daifclr（IRQ 使能）之后，中断才会真正到达 CPU。
     */
    arch_timer_set_next_event_ns(10000000ULL);
}
```

### 修改文件: `arch/arm64/kernel/entry.S` (Phase 3 版本)

> Phase 3 更新：`el1h_irq` 和 `el0_irq` 现在调用 `handle_irq` 而不是 `panic_unhandled`。
> `el0_sync` 仍然调用 `panic_unhandled`（Phase 5 才添加 SVC 分发）。

```asm
/* SPDX-License-Identifier: GPL-2.0 */
/*
 * arm64os/arch/arm64/kernel/entry.S
 *
 * ARM64 异常向量表与上下文保存/恢复（Phase 3 版本）
 */

/* ---- pt_regs 偏移常量 ---- */
#define PT_REGS_SIZE    272         /* 34 x 8，16字节对齐 */
#define PT_LR           (30 * 8)   /* 240: x30 / Link Register */
#define PT_SP           (31 * 8)   /* 248: 用户态 SP_EL0 */
#define PT_PC           (32 * 8)   /* 256: 异常返回地址（ELR_EL1）*/
#define PT_PSTATE       (33 * 8)   /* 264: 保存的处理器状态（SPSR_EL1）*/

/*
 * kernel_entry 宏 — 保存所有寄存器到栈上 pt_regs
 * 参数 el: 0=来自EL0（用户态），1=来自EL1（内核态）
 */
.macro  kernel_entry, el
    sub     sp, sp, #PT_REGS_SIZE

    stp     x0,  x1,  [sp, #16 * 0]
    stp     x2,  x3,  [sp, #16 * 1]
    stp     x4,  x5,  [sp, #16 * 2]
    stp     x6,  x7,  [sp, #16 * 3]
    stp     x8,  x9,  [sp, #16 * 4]
    stp     x10, x11, [sp, #16 * 5]
    stp     x12, x13, [sp, #16 * 6]
    stp     x14, x15, [sp, #16 * 7]
    stp     x16, x17, [sp, #16 * 8]
    stp     x18, x19, [sp, #16 * 9]
    stp     x20, x21, [sp, #16 * 10]
    stp     x22, x23, [sp, #16 * 11]
    stp     x24, x25, [sp, #16 * 12]
    stp     x26, x27, [sp, #16 * 13]
    stp     x28, x29, [sp, #16 * 14]
    str     x30,      [sp, #PT_LR]

    mrs     x21, elr_el1
    mrs     x22, spsr_el1
    stp     x21, x22, [sp, #PT_PC]

    .if \el == 0
    mrs     x21, sp_el0
    str     x21, [sp, #PT_SP]
    .endif
.endm

/*
 * kernel_exit 宏 — 从 pt_regs 恢复寄存器并 eret 返回
 */
.macro  kernel_exit, el
    .if \el == 0
    ldr     x21, [sp, #PT_SP]
    msr     sp_el0, x21
    .endif

    ldp     x21, x22, [sp, #PT_PC]
    msr     elr_el1,  x21
    msr     spsr_el1, x22

    ldp     x0,  x1,  [sp, #16 * 0]
    ldp     x2,  x3,  [sp, #16 * 1]
    ldp     x4,  x5,  [sp, #16 * 2]
    ldp     x6,  x7,  [sp, #16 * 3]
    ldp     x8,  x9,  [sp, #16 * 4]
    ldp     x10, x11, [sp, #16 * 5]
    ldp     x12, x13, [sp, #16 * 6]
    ldp     x14, x15, [sp, #16 * 7]
    ldp     x16, x17, [sp, #16 * 8]
    ldp     x18, x19, [sp, #16 * 9]
    ldp     x20, x21, [sp, #16 * 10]
    ldp     x22, x23, [sp, #16 * 11]
    ldp     x24, x25, [sp, #16 * 12]
    ldp     x26, x27, [sp, #16 * 13]
    ldp     x28, x29, [sp, #16 * 14]
    ldr     x30,      [sp, #PT_LR]

    add     sp, sp, #PT_REGS_SIZE
    eret
.endm

/* 向量入口宏 */
.macro  ventry  label
    .align  7
    b       \label
.endm

/* ============================================================ */
/* 异常向量表（2KB 对齐，16 个入口 x 128 字节）               */
/* ============================================================ */
    .section ".text", "ax"

    .align  11
    .global vectors
vectors:
    /* Group 1: 当前 EL，SP_EL0（配置错误）*/
    ventry  el1t_sync
    ventry  el1t_irq
    ventry  el1t_fiq
    ventry  el1t_error

    /* Group 2: 当前 EL，SP_EL1（内核态正常路径）*/
    ventry  el1h_sync
    ventry  el1h_irq
    ventry  el1h_fiq
    ventry  el1h_error

    /* Group 3: 来自 EL0（用户态），AArch64 */
    ventry  el0_sync
    ventry  el0_irq
    ventry  el0_fiq
    ventry  el0_error

    /* Group 4: 来自 EL0，AArch32（不支持）*/
    ventry  el0_32_sync
    ventry  el0_32_irq
    ventry  el0_32_fiq
    ventry  el0_32_error

/* ---- 异常处理函数 ---- */

/* EL1 SP_EL0：配置错误，挂死 */
el1t_sync:
el1t_irq:
el1t_fiq:
el1t_error:
    kernel_entry 1
    bl      panic_unhandled
    b       .

/* EL1 SP_EL1 同步异常（内核态） */
el1h_sync:
    kernel_entry 1
    mov     x0, sp
    bl      handle_sync_exception
    kernel_exit 1

/* EL1 SP_EL1 IRQ — Phase 3: 调用 handle_irq */
el1h_irq:
    kernel_entry 1
    mov     x0, sp
    bl      handle_irq
    kernel_exit 1

/* EL1 FIQ / SError */
el1h_fiq:
el1h_error:
    kernel_entry 1
    bl      panic_unhandled
    b       .

/* EL0 同步异常 — Phase 3: 仍调用 panic_unhandled（Phase 5 添加 SVC 分发）*/
el0_sync:
    kernel_entry 0
    bl      panic_unhandled
    b       .

/* EL0 IRQ — Phase 3: 调用 handle_irq */
el0_irq:
    kernel_entry 0
    mov     x0, sp
    bl      handle_irq
    kernel_exit 0

/* EL0 FIQ / SError — 不支持 */
el0_fiq:
el0_error:
    kernel_entry 0
    bl      panic_unhandled
    b       .

/* AArch32 — 不支持 */
el0_32_sync:
el0_32_irq:
el0_32_fiq:
el0_32_error:
    kernel_entry 1
    bl      panic_unhandled
    b       .
```

### 修改文件: `kernel/main.c` (Phase 3 版本)

> 包含 Phase 1 + Phase 2 + Phase 3 全部代码。`start_kernel()` 以 "Phase 3 complete" 结尾。

```c
/* SPDX-License-Identifier: GPL-2.0 */
/*
 * arm64os/kernel/main.c — Phase 3 版本
 *
 * 内核主入口及异常处理
 *
 * Phase 1: 启动信息 + 异常向量表验证
 * Phase 2: MMU + memblock + Buddy
 * Phase 3: GIC v3 + arch timer + IRQ 验证
 *
 * 注：handle_irq() 已移至 kernel/irq/handle.c
 */

#include <linux/types.h>
#include <asm/memory.h>

/* 由 printk.c 提供 */
void boot_printk(const char *s);
void boot_printk_hex(unsigned long val);

/* Phase 2 */
void mmu_init(void);
void memblock_init(phys_addr_t phys_start, phys_addr_t phys_size);
void buddy_init(void);
void test_buddy(void);

/* Phase 3 */
void gicv3_init(void);
void arch_timer_init(void);
extern volatile int arch_timer_tick_count;

/* 由 linker script 定义的符号 */
extern char _text[];
extern char _end[];
extern char _bss_start[];
extern char _bss_end[];

/* 由 head.S 定义 */
extern unsigned long boot_args[4];

/*
 * pt_regs - 异常现场
 */
struct pt_regs {
    unsigned long regs[31];
    unsigned long sp;
    unsigned long pc;
    unsigned long pstate;
};

static const char *esr_to_str(unsigned long esr)
{
    unsigned int ec = (esr >> 26) & 0x3f;
    switch (ec) {
    case 0x00: return "Unknown reason";
    case 0x01: return "WFI/WFE instruction";
    case 0x0e: return "Illegal Execution State";
    case 0x15: return "SVC (AArch64 syscall)";
    case 0x20: return "Instruction Abort (lower EL)";
    case 0x21: return "Instruction Abort (current EL)";
    case 0x24: return "Data Abort (lower EL)";
    case 0x25: return "Data Abort (current EL)";
    case 0x26: return "SP alignment fault";
    default:   return "Unknown EC";
    }
}

void handle_sync_exception(struct pt_regs *regs)
{
    unsigned long esr, far;
    __asm__ volatile("mrs %0, esr_el1" : "=r"(esr));
    __asm__ volatile("mrs %0, far_el1" : "=r"(far));

    boot_printk("\n[EXCEPTION] Synchronous exception!\n");
    boot_printk("  ESR_EL1: ");
    boot_printk_hex(esr);
    boot_printk(" (");
    boot_printk(esr_to_str(esr));
    boot_printk(")\n");
    boot_printk("  FAR_EL1: ");
    boot_printk_hex(far);
    boot_printk("\n  PC     : ");
    boot_printk_hex(regs->pc);
    boot_printk("\n");

    boot_printk("[PANIC] Unrecoverable — halting.\n");
    while (1);
}

void panic_unhandled(void)
{
    unsigned long esr, far;
    __asm__ volatile("mrs %0, esr_el1" : "=r"(esr));
    __asm__ volatile("mrs %0, far_el1" : "=r"(far));

    boot_printk("\n[PANIC] Unhandled exception!\n");
    boot_printk("  ESR_EL1: ");
    boot_printk_hex(esr);
    boot_printk("\n  FAR_EL1: ");
    boot_printk_hex(far);
    boot_printk("\n");
    while (1);
}

/*
 * scheduler_tick - Phase 3 桩函数（Phase 4 实现）
 */
void scheduler_tick(void)
{
    /* Phase 3: 空操作，Phase 4 将实现 CFS tick */
}

void start_kernel(void)
{
    boot_printk("[BOOT] ARM64 kernel starting...\n");
    boot_printk("[BOOT] Phase 3: GIC v3 + arch timer + IRQ\n");

    /* ---- Phase 1: 启动信息 ---- */
    boot_printk("[BOOT] Kernel text   : ");
    boot_printk_hex((unsigned long)_text);
    boot_printk("\n");
    boot_printk("[BOOT] Kernel end    : ");
    boot_printk_hex((unsigned long)_end);
    boot_printk("\n");
    boot_printk("[BOOT] BSS           : ");
    boot_printk_hex((unsigned long)_bss_start);
    boot_printk(" - ");
    boot_printk_hex((unsigned long)_bss_end);
    boot_printk("\n");
    boot_printk("[BOOT] FDT addr      : ");
    boot_printk_hex(boot_args[0]);
    boot_printk("\n");

    {
        unsigned long vbar;
        __asm__ volatile("mrs %0, vbar_el1" : "=r"(vbar));
        boot_printk("[BOOT] VBAR_EL1      : ");
        boot_printk_hex(vbar);
        boot_printk("\n");
    }

    /* ---- Phase 2: MMU 初始化 ---- */
    boot_printk("[BOOT] Initializing MMU (identity mapping)...\n");
    mmu_init();
    boot_printk("[BOOT] MMU enabled (SCTLR_EL1.M = 1)\n");

    {
        unsigned long sctlr;
        __asm__ volatile("mrs %0, sctlr_el1" : "=r"(sctlr));
        boot_printk("[BOOT] SCTLR_EL1     : ");
        boot_printk_hex(sctlr);
        boot_printk(" (M=");
        boot_printk((sctlr & 1) ? "1" : "0");
        boot_printk(")\n");
    }

    /* ---- Phase 2: 物理内存初始化 ---- */
    boot_printk("[BOOT] Initializing memblock...\n");
    memblock_init(PHYS_OFFSET, PHYS_SIZE);

    boot_printk("[BOOT] Initializing buddy allocator...\n");
    buddy_init();

    /* ---- Phase 2: 验证 Buddy ---- */
    test_buddy();

    /* ---- Phase 3: GIC v3 初始化 ---- */
    boot_printk("[BOOT] Initializing GIC v3...\n");
    gicv3_init();
    boot_printk("[BOOT] GIC v3 initialized\n");

    /* ---- Phase 3: arch timer 初始化 ---- */
    boot_printk("[BOOT] Initializing arch timer (Virtual Timer PPI #27)...\n");
    arch_timer_init();
    boot_printk("[BOOT] arch timer started (10ms interval)\n");

    /* ---- Phase 3: 使能 IRQ ---- */
    boot_printk("[BOOT] Enabling IRQ (daifclr #2)...\n");
    __asm__ volatile("msr daifclr, #2" ::: "memory");

    /* ---- Phase 3: 验证 timer tick ---- */
    boot_printk("[BOOT] Waiting for 10 timer ticks...\n");
    while (arch_timer_tick_count < 10)
        ;

    boot_printk("[BOOT] Timer ticks: OK (received >= 10)\n");
    boot_printk("[BOOT] Phase 3 complete\n");

    while (1)
        ;
}
```

### 修改文件: `Makefile` (Phase 3 版本)

> OBJS 新增 5 个目标文件，新增 4 条编译规则。

OBJS 修改为:
```makefile
OBJS := \
    arch/arm64/kernel/head.o \
    arch/arm64/kernel/entry.o \
    arch/arm64/mm/proc.o \
    arch/arm64/mm/tlb.o \
    arch/arm64/mm/mmu.o \
    mm/memblock.o \
    mm/page_alloc.o \
    kernel/printk.o \
    kernel/irq/irqdesc.o \
    kernel/irq/handle.o \
    drivers/irqchip/gic-v3.o \
    drivers/timer/arm_arch_timer.o \
    kernel/main.o
```

新增编译规则:
```makefile
# Phase 3: kernel/irq/ C 文件
kernel/irq/%.o: kernel/irq/%.c
	$(CC) $(CFLAGS) -c -o $@ $<

# Phase 3: drivers/irqchip/ C 文件
drivers/irqchip/%.o: drivers/irqchip/%.c
	$(CC) $(CFLAGS) -c -o $@ $<

# Phase 3: drivers/timer/ C 文件
drivers/timer/%.o: drivers/timer/%.c
	$(CC) $(CFLAGS) -c -o $@ $<
```

### 编译运行

```bash
# 创建新目录
mkdir -p kernel/irq drivers/irqchip drivers/timer

make clean && make
make run
# 期望输出包含：
#   [BOOT] GIC v3 initialized
#   [BOOT] arch timer started (10ms interval)
#   [BOOT] Enabling IRQ (daifclr #2)...
#   [BOOT] Waiting for 10 timer ticks...
#   [BOOT] Timer ticks: OK (received >= 10)
#   [BOOT] Phase 3 complete
# 按 Ctrl-A X 退出 QEMU
```
