# Phase 4：CFS完全公平调度器

## 参考内核文件

```
kernel/sched/core.c                  # 调度器核心（context_switch）
kernel/sched/fair.c                  # CFS公平调度
kernel/sched/sched.h                 # 调度器内部数据结构
arch/arm64/kernel/process.c         # cpu_switch_to（上下文切换汇编）
arch/arm64/kernel/asm-offsets.c     # 生成汇编中用的结构体偏移量
```

---

## 4.1 核心数据结构（参考 kernel/sched/sched.h）

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

## 4.2 vruntime 计算（CFS核心公式）

```c
/*
 * vruntime是CFS的核心概念：
 * vruntime += actual_runtime × (NICE_0_WEIGHT / task_weight)
 *
 * 高优先级（大weight）进程的vruntime增长慢，
 * 低优先级进程vruntime增长快，CFS始终选择vruntime最小的进程运行。
 *
 * 参考 kernel/sched/fair.c : __update_curr()
 */

#define NICE_0_WEIGHT   1024    /* nice=0时的标准权重 */

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

    update_min_vruntime(cfs_rq);
}
```

## 4.3 进程入队/出队（参考 fair.c enqueue_entity/dequeue_entity）

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

## 4.4 上下文切换（ARM64汇编，参考 arch/arm64/kernel/process.c）

```asm
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
    ldp     x29, x9,  [x8], #16
    ldr     x10, [x8]
    mov     sp, x10

    /* 切换页表（如果进程有自己的地址空间）*/
    ldr     x10, [x1, #TASK_MM]
    cbz     x10, switch_to_kernel
    ldr     x10, [x10, #MM_PGD]
    msr     ttbr0_el1, x10
    isb
    tlbi    vmalle1is
    dsb     ish
    isb

switch_to_kernel:
    br      x9
```

## 4.5 调度器时钟中断处理

```c
/* 参考 kernel/sched/core.c : scheduler_tick() */
void scheduler_tick(void) {
    struct rq *rq = this_rq();
    struct task_struct *curr = rq->curr;

    update_curr(&rq->cfs);

    if (cfs_rq->nr_running > 1) {
        struct sched_entity *se = pick_next_entity(&rq->cfs);
        if (curr->se.vruntime - se->vruntime > sched_latency / nr_running)
            resched_curr(rq);  /* 设置 TIF_NEED_RESCHED 标志 */
    }
}

void schedule(void) {
    struct task_struct *prev = current;
    struct task_struct *next = pick_next_task(&runqueue);

    if (next != prev) {
        prev->se.exec_start = 0;
        context_switch(prev, next);  /* 调用 cpu_switch_to */
    }
}
```

## 4.6 验证方法

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

## 4.7 本阶段产出文件

```
arm64os/
├── kernel/sched/
│   ├── core.c                    ← 调度器核心、context_switch（核心）
│   ├── fair.c                    ← CFS实现（核心）
│   └── run_queue.c               ← 运行队列管理
└── arch/arm64/kernel/
    └── process.c                 ← cpu_switch_to汇编（核心）
```
