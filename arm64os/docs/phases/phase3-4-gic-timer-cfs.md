# Phase 3 & 4：GIC v3中断控制器 + arch timer + CFS调度器

## 参考内核文件

```
drivers/irqchip/irq-gic-v3.c        # GIC v3驱动（主要实现）
drivers/irqchip/irq-gic-v3-its.c    # ITS（中断转发服务，可简化跳过）
include/linux/irqchip/arm-gic-v3.h  # GIC v3寄存器定义
drivers/clocksource/arm_arch_timer.c # ARM通用计时器驱动
include/clocksource/arm_arch_timer.h # 计时器寄存器定义
kernel/sched/core.c                  # 调度器核心（context_switch）
kernel/sched/fair.c                  # CFS公平调度
kernel/sched/sched.h                 # 调度器内部数据结构
arch/arm64/kernel/entry.S           # 中断入口（el1h_irq, el0_irq）
arch/arm64/kernel/process.c         # cpu_switch_to（上下文切换汇编）
arch/arm64/kernel/asm-offsets.c     # 生成汇编中用的结构体偏移量
```

---

## Phase 3：GIC v3 中断控制器

### 3.1 GIC v3架构概览

GIC v3由两部分组成：
- **Distributor（GICD）**：全局，管理所有中断的使能/优先级/路由，MMIO地址一般为 `0x08000000`
- **Redistributor（GICR）**：每CPU一个，管理PPI和SGI，通常在 `0x080A0000`

中断分类：
```
SGI  (0-15)   : Software Generated Interrupts，核间通信
PPI  (16-31)  : Per-CPU Private Interrupts，如arch timer (PPI #30)
SPI  (32-1019): Shared Peripheral Interrupts，设备中断
```

### 3.2 GIC v3寄存器映射（参考 arm-gic-v3.h）

```c
/* GICD寄存器偏移（参考 include/linux/irqchip/arm-gic-v3.h）*/
#define GICD_CTLR           0x0000  /* 分发器控制 */
#define GICD_TYPER          0x0004  /* 类型寄存器（中断数量等）*/
#define GICD_ISENABLER      0x0100  /* SPI中断使能设置（每位一个中断）*/
#define GICD_ICENABLER      0x0180  /* SPI中断使能清除 */
#define GICD_ISPENDR        0x0200  /* pending设置 */
#define GICD_IPRIORITYR     0x0400  /* 中断优先级（每字节一个中断）*/
#define GICD_ITARGETSR      0x0800  /* GICv2 CPU目标（v3废弃，用路由）*/
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

### 3.3 GIC v3初始化流程

```c
/* 参考 drivers/irqchip/irq-gic-v3.c : gic_init_bases() */

void gicv3_init(void *gicd_base, void *gicr_base) {
    /* Step 1: Distributor初始化 */
    /* 1.1 禁用Distributor */
    writel(0, gicd_base + GICD_CTLR);
    /* 等待写完成 */
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
    while (readl(gicr_base + GICR_WAKER) & (1 << 2)); /* 等待ChildrenAsleep=0 */

    /* Step 3: CPU接口初始化（通过系统寄存器）*/
    /* 3.1 使能系统寄存器接口 */
    asm volatile("msr ICC_SRE_EL1, %0" :: "r"(7UL));
    asm volatile("isb");

    /* 3.2 设置优先级掩码（允许所有中断）*/
    asm volatile("msr ICC_PMR_EL1, %0" :: "r"(0xFFUL));

    /* 3.3 设置Binary Point（不使用抢占）*/
    asm volatile("msr ICC_BPR1_EL1, %0" :: "r"(0UL));

    /* 3.4 使能Group 1中断 */
    asm volatile("msr ICC_IGRPEN1_EL1, %0" :: "r"(1UL));
    asm volatile("isb");
}

/* 中断处理主循环（在entry.S的IRQ handler中调用）*/
void handle_irq(void) {
    uint64_t irqnr;
    /* 读取中断号（同时标记为Active）*/
    asm volatile("mrs %0, ICC_IAR1_EL1" : "=r"(irqnr));

    if (irqnr < 1020) {
        /* 调用中断处理函数 */
        irq_handlers[irqnr]();

        /* 写EOIR标记处理完成 */
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

---

## Phase 3：ARM64 通用计时器（arch timer）

### 3.4 ARMv8计时器系统寄存器（参考 arm_arch_timer.h）

```
系统中有4个计时器，每CPU各自拥有：

EL1 Physical Timer:
  CNTP_CTL_EL0  - 控制寄存器（ENABLE/IMASK/ISTATUS）
  CNTP_TVAL_EL0 - TimerValue（倒计时值）
  CNTP_CVAL_EL0 - CompareValue（绝对比较值）

EL1 Virtual Timer (我们主要使用这个):
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

```c
/* 参考 drivers/clocksource/arm_arch_timer.c */

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

    /* 使用TVAL（倒计时）方式 */
    asm volatile("msr cntv_tval_el0, %0" :: "r"(ticks));

    /* 使能计时器，不屏蔽中断 */
    asm volatile("msr cntv_ctl_el0, %0" :: "r"((uint64_t)ARCH_TIMER_CTL_ENABLE));
    asm volatile("isb");
}

/* 计时器中断处理 */
void arch_timer_handler(void) {
    /* 屏蔽计时器（避免重复触发），然后重新设置 */
    asm volatile("msr cntv_ctl_el0, %0" ::
                 "r"((uint64_t)(ARCH_TIMER_CTL_ENABLE | ARCH_TIMER_CTL_IMASK)));

    /* 触发调度（tick）*/
    scheduler_tick();

    /* 重新设置下次中断（HZ=100, 10ms）*/
    arch_timer_set_next_event_ns(10000000ULL);
}
```

---

## Phase 4：CFS完全公平调度器

### 4.1 核心数据结构（参考 kernel/sched/sched.h）

```c
/* 每个进程的调度实体（参考 include/linux/sched.h）*/
struct sched_entity {
    struct load_weight  load;           /* 权重（决定分配CPU时间比例）*/
    struct rb_node      run_node;       /* 在红黑树中的节点 */
    uint64_t            vruntime;       /* 虚拟运行时间（核心！）*/
    uint64_t            exec_start;     /* 本次执行开始的物理时间 */
    uint64_t            sum_exec_runtime; /* 累计实际运行时间 */
};

/* CFS运行队列（每CPU每调度域各一个）*/
struct cfs_rq {
    struct load_weight  load;          /* 队列总权重 */
    unsigned int        nr_running;    /* 可运行进程数 */
    uint64_t            min_vruntime;  /* 队列中最小vruntime（关键！）*/
    struct rb_root_cached tasks_timeline; /* 红黑树（按vruntime排序）*/
};

/* 进程控制块（简化版 task_struct）*/
struct task_struct {
    volatile long       state;         /* 运行状态 */
    void               *stack;         /* 内核栈 */
    pid_t               pid;
    struct sched_entity se;            /* CFS调度实体 */
    struct mm_struct   *mm;            /* 内存描述符（内核线程为NULL）*/
    /* 上下文切换保存的寄存器（参考 arch/arm64/include/asm/processor.h）*/
    struct cpu_context {
        unsigned long x19, x20, x21, x22, x23, x24, x25, x26, x27, x28;
        unsigned long fp, sp, pc;
    } thread;
};
```

### 4.2 vruntime 计算（CFS核心公式）

```c
/*
 * vruntime是CFS的核心概念：
 * vruntime += actual_runtime × (NICE_0_WEIGHT / task_weight)
 *
 * 这样高优先级（大weight）进程的vruntime增长慢，
 * 低优先级进程vruntime增长快，CFS始终选择vruntime最小的进程运行。
 *
 * 参考 kernel/sched/fair.c : __update_curr()
 */

#define NICE_0_WEIGHT   1024    /* nice=0时的标准权重 */
#define WMULT_SHIFT     32

/* nice值到权重的映射表（参考 kernel/sched/core.c sched_prio_to_weight[]）*/
static const int sched_prio_to_weight[40] = {
 /* -20 */     88761, 71755, 56483, 46273, 36291,
 /* -15 */     29154, 23254, 18705, 14949, 11916,
 /* -10 */      9548,  7620,  6100,  4904,  3906,
 /*  -5 */      3121,  2501,  1991,  1586,  1277,
 /*   0 */      1024,   820,   655,   526,   423,
 /*   5 */       335,   272,   215,   172,   137,
 /*  10 */       110,    87,    70,    56,    45,
 /*  15 */        36,    29,    23,    18,    15,
};

static void update_curr(struct cfs_rq *cfs_rq) {
    struct sched_entity *curr = cfs_rq->curr;
    uint64_t now = sched_clock_cpu();  /* 读取CNTVCT_EL0 */
    uint64_t delta_exec;

    delta_exec = now - curr->exec_start;
    curr->exec_start = now;
    curr->sum_exec_runtime += delta_exec;

    /* vruntime += delta * NICE_0_WEIGHT / weight */
    uint64_t delta_vruntime =
        (delta_exec * NICE_0_WEIGHT) / curr->load.weight;
    curr->vruntime += delta_vruntime;

    /* 更新队列的min_vruntime */
    update_min_vruntime(cfs_rq);
}
```

### 4.3 进程入队/出队（参考 fair.c enqueue_entity/dequeue_entity）

```c
/* 将进程插入红黑树（按vruntime排序）*/
static void __enqueue_entity(struct cfs_rq *cfs_rq, struct sched_entity *se) {
    struct rb_node **link = &cfs_rq->tasks_timeline.rb_root.rb_node;
    struct rb_node *parent = NULL;
    struct sched_entity *entry;
    bool leftmost = true;

    while (*link) {
        parent = *link;
        entry = rb_entry(parent, struct sched_entity, run_node);
        if (se->vruntime < entry->vruntime) {
            link = &parent->rb_left;
        } else {
            link = &parent->rb_right;
            leftmost = false;
        }
    }
    rb_link_node(&se->run_node, parent, link);
    rb_insert_color_cached(&se->run_node, &cfs_rq->tasks_timeline, leftmost);
}

/* 选择下一个运行的进程（最左节点，即vruntime最小的）*/
static struct sched_entity *pick_next_entity(struct cfs_rq *cfs_rq) {
    struct rb_node *left = rb_first_cached(&cfs_rq->tasks_timeline);
    if (!left) return NULL;
    return rb_entry(left, struct sched_entity, run_node);
}
```

### 4.4 上下文切换（ARM64汇编，参考 arch/arm64/kernel/process.c）

```asm
/* 参考 arch/arm64/kernel/entry.S 和 process.c 中的 cpu_switch_to */
/*
 * cpu_switch_to(prev, next)
 * x0 = prev task_struct
 * x1 = next task_struct
 *
 * 保存 prev 的被调用者保存寄存器（x19-x28, fp, sp, pc）
 * 恢复 next 的寄存器
 */
.global cpu_switch_to
cpu_switch_to:
    /* thread.cpu_context 在 task_struct 中的偏移 */
    add     x8, x0, #THREAD_CPU_CONTEXT

    /* 保存 callee-saved 寄存器到 prev->thread.cpu_context */
    stp     x19, x20, [x8], #16
    stp     x21, x22, [x8], #16
    stp     x23, x24, [x8], #16
    stp     x25, x26, [x8], #16
    stp     x27, x28, [x8], #16
    stp     x29, x9,  [x8], #16    /* x9 = lr (返回地址) */
    mov     x9, sp
    str     x9,  [x8]              /* 保存 sp */

    /* 从 next->thread.cpu_context 恢复寄存器 */
    add     x8, x1, #THREAD_CPU_CONTEXT
    ldp     x19, x20, [x8], #16
    ldp     x21, x22, [x8], #16
    ldp     x23, x24, [x8], #16
    ldp     x25, x26, [x8], #16
    ldp     x27, x28, [x8], #16
    ldp     x29, x9,  [x8], #16    /* x29 = fp, x9 = pc */
    ldr     x10, [x8]              /* sp */
    mov     sp, x10

    /* 切换页表（如果进程有自己的地址空间）*/
    ldr     x10, [x1, #TASK_MM]    /* next->mm */
    cbz     x10, switch_to_kernel  /* 内核线程：不切换页表 */
    ldr     x10, [x10, #MM_PGD]    /* mm->pgd */
    msr     ttbr0_el1, x10
    isb
    tlbi    vmalle1is               /* 无效化ASID 0的TLB（简化版）*/
    dsb     ish
    isb

switch_to_kernel:
    /* 跳转到next进程上次被调度出去的位置 */
    br      x9
```

### 4.5 调度器时钟中断处理

```c
/* 参考 kernel/sched/core.c : scheduler_tick() */
void scheduler_tick(void) {
    struct rq *rq = this_rq();         /* 当前CPU的运行队列 */
    struct task_struct *curr = rq->curr;

    /* 更新当前进程的vruntime */
    update_curr(&rq->cfs);

    /* 检查是否需要抢占（当前进程是否还是最应该运行的？）*/
    if (cfs_rq->nr_running > 1) {
        struct sched_entity *se = pick_next_entity(&rq->cfs);
        /* 如果最小vruntime的进程比当前进程小太多，触发抢占 */
        if (curr->se.vruntime - se->vruntime > sched_latency / nr_running)
            resched_curr(rq);  /* 设置 TIF_NEED_RESCHED 标志 */
    }
}

/* 在中断返回时检查是否需要重新调度 */
/* 参考 entry.S 的 ret_to_user / el1_irq_handler */
void schedule(void) {
    struct task_struct *prev = current;
    struct task_struct *next = pick_next_task(&runqueue);

    if (next != prev) {
        prev->se.exec_start = 0;
        context_switch(prev, next);  /* 调用 cpu_switch_to */
    }
}
```

### 4.6 阶段验证

```c
/* 创建3个优先级不同的内核线程，验证CFS调度 */
void test_scheduler(void) {
    /* 线程A: nice=0, 权重1024 */
    /* 线程B: nice=5, 权重335 */
    /* 线程C: nice=-5, 权重3121 */

    /* 运行10秒后统计各线程实际运行时间 */
    /* 理论比例: A:B:C ≈ 1024:335:3121 ≈ 23%:7.5%:70% */

    printk("Thread A runtime: %llu ns\n", thread_a->se.sum_exec_runtime);
    printk("Thread B runtime: %llu ns\n", thread_b->se.sum_exec_runtime);
    printk("Thread C runtime: %llu ns\n", thread_c->se.sum_exec_runtime);
}
```
