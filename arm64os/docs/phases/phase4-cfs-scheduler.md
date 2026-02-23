# Phase 4：CFS完全公平调度器

## 参考内核文件

```
kernel/sched/core.c                  # 调度器核心（schedule, context_switch, scheduler_tick）
kernel/sched/fair.c                  # CFS公平调度（enqueue/dequeue/pick, update_curr）
include/linux/sched.h                # 进程控制块、调度实体、运行队列数据结构
include/linux/rbtree.h               # 红黑树接口
lib/rbtree.c                         # 红黑树实现
arch/arm64/kernel/process.S          # cpu_switch_to（上下文切换汇编）
```

---

## 4.1 核心数据结构（参考 kernel/sched/sched.h）

```c
/* 权重结构体 */
struct load_weight {
    unsigned long weight;              /* nice值对应的权重 */
};

/* 红黑树节点（简化版，参考 include/linux/rbtree.h）*/
struct rb_node {
    unsigned long  __rb_parent_color;  /* 父节点指针+颜色位（最低bit）*/
    struct rb_node *rb_right;
    struct rb_node *rb_left;
};

struct rb_root {
    struct rb_node *rb_node;           /* 根节点 */
};

/* 带缓存的红黑树根（缓存最左节点，O(1)取最小值）*/
struct rb_root_cached {
    struct rb_root  rb_root;
    struct rb_node *rb_leftmost;       /* 缓存vruntime最小的节点 */
};

/* 每个进程的调度实体（参考 include/linux/sched.h）*/
struct sched_entity {
    struct load_weight  load;           /* 权重（决定分配CPU时间比例）*/
    struct rb_node      run_node;       /* 在红黑树中的节点 */
    unsigned int        on_rq;          /* 是否在运行队列中 */
    uint64_t            vruntime;       /* 虚拟运行时间（核心！）*/
    uint64_t            exec_start;     /* 本次执行开始的物理时间 */
    uint64_t            sum_exec_runtime; /* 累计实际运行时间 */
};

/* CFS运行队列（每CPU一个，Phase 4 单CPU）*/
struct cfs_rq {
    struct load_weight  load;           /* 队列总权重 */
    unsigned int        nr_running;     /* 可运行进程数 */
    uint64_t            min_vruntime;   /* 队列中最小vruntime（新进程入队基准）*/
    struct sched_entity *curr;          /* 当前运行的调度实体 */
    struct rb_root_cached tasks_timeline; /* 红黑树（按vruntime排序）*/
};

/* 每CPU运行队列（封装cfs_rq + 当前任务指针）*/
struct rq {
    struct task_struct  *curr;          /* 当前运行的进程 */
    struct task_struct  *idle;          /* idle进程（无其他可运行进程时运行）*/
    struct cfs_rq       cfs;            /* CFS运行队列 */
    unsigned int        nr_switches;    /* 上下文切换计数（调试用）*/
};

/* 进程状态 */
#define TASK_RUNNING        0           /* 正在运行或在运行队列中 */
#define TASK_DEAD           4           /* 已退出 */

/* 调度标志 */
#define TIF_NEED_RESCHED    (1UL << 0)  /* 需要重新调度 */

/* cpu_context：上下文切换时保存的寄存器 */
struct cpu_context {
    unsigned long x19, x20, x21, x22, x23;
    unsigned long x24, x25, x26, x27, x28;
    unsigned long fp;                    /* x29 */
    unsigned long sp;
    unsigned long pc;                    /* 返回地址（LR） */
};

/* 进程控制块（简化版 task_struct）*/
struct task_struct {
    volatile long       state;          /* 运行状态（TASK_RUNNING 等）*/
    unsigned long       flags;          /* TIF_NEED_RESCHED 等标志 */
    void               *stack;          /* 内核栈底（低地址端）*/
    int                 pid;            /* 进程ID */
    int                 prio;           /* 优先级（nice值 + 20，范围0-39）*/
    char                comm[16];       /* 进程名（调试用）*/
    struct sched_entity se;             /* CFS调度实体 */
    struct cpu_context  thread;         /* 上下文切换保存区 */
    /* mm_struct *mm 省略：Phase 4 全部为内核线程，mm=NULL */
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

/* sched_clock() - 读取ARM物理计数器作为调度时钟（纳秒）*/
static uint64_t sched_clock(void) {
    uint64_t cnt, freq;
    __asm__ volatile("mrs %0, cntvct_el0" : "=r"(cnt));
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(freq));
    return (cnt * 1000000000ULL) / freq;
}

static void update_curr(struct cfs_rq *cfs_rq) {
    struct sched_entity *curr = cfs_rq->curr;
    uint64_t now = sched_clock();
    uint64_t delta_exec;

    if (!curr)
        return;

    delta_exec = now - curr->exec_start;
    curr->exec_start = now;
    curr->sum_exec_runtime += delta_exec;

    /* vruntime += delta * NICE_0_WEIGHT / weight */
    curr->vruntime += (delta_exec * NICE_0_WEIGHT) / curr->load.weight;

    /* 更新 min_vruntime（单调递增） */
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
        if ((s64)(se->vruntime - entry->vruntime) < 0) {
            link = &parent->rb_left;
        } else {
            link = &parent->rb_right;
            leftmost = false;
        }
    }
    rb_link_node(&se->run_node, parent, link);
    rb_insert_color_cached(&se->run_node, &cfs_rq->tasks_timeline, leftmost);
}

/* 从红黑树移除进程 */
static void __dequeue_entity(struct cfs_rq *cfs_rq, struct sched_entity *se) {
    rb_erase_cached(&se->run_node, &cfs_rq->tasks_timeline);
}

/* 选择下一个运行的进程（最左节点，即vruntime最小的）*/
static struct sched_entity *pick_next_entity(struct cfs_rq *cfs_rq) {
    struct rb_node *left = rb_first_cached(&cfs_rq->tasks_timeline);
    if (!left) return NULL;
    return rb_entry(left, struct sched_entity, run_node);
}
```

## 4.4 上下文切换（ARM64汇编）

```asm
/*
 * cpu_switch_to(prev, next)
 * x0 = prev task_struct
 * x1 = next task_struct
 * 返回值：x0 = prev（保持不变，方便调用者使用）
 *
 * 保存 prev 的 callee-saved 寄存器（x19-x28, fp, sp, lr）
 * 恢复 next 的寄存器
 * 通过 ret 指令跳转到 next 的恢复地址
 *
 * THREAD_CPU_CONTEXT = offsetof(struct task_struct, thread)
 * 该值需与 include/linux/sched.h 中 struct task_struct 的布局匹配。
 *
 * Phase 4 所有任务为内核线程（mm=NULL），不切换页表。
 */
.global cpu_switch_to
cpu_switch_to:
    mov     x10, #THREAD_CPU_CONTEXT
    add     x8, x0, x10

    /* 保存 callee-saved 寄存器到 prev->thread */
    stp     x19, x20, [x8], #16
    stp     x21, x22, [x8], #16
    stp     x23, x24, [x8], #16
    stp     x25, x26, [x8], #16
    stp     x27, x28, [x8], #16
    stp     x29, x9,  [x8], #16    /* fp + lr(暂存到x9) */
    mov     x9, lr
    stp     x29, x9, [x8, #-16]    /* 重写: fp, lr */
    mov     x9, sp
    str     x9,  [x8]              /* 保存 sp */

    /* 从 next->thread 恢复寄存器 */
    mov     x10, #THREAD_CPU_CONTEXT
    add     x8, x1, x10
    ldp     x19, x20, [x8], #16
    ldp     x21, x22, [x8], #16
    ldp     x23, x24, [x8], #16
    ldp     x25, x26, [x8], #16
    ldp     x27, x28, [x8], #16
    ldp     x29, x9,  [x8], #16    /* fp + pc(返回地址) */
    ldr     x10, [x8]
    mov     sp, x10

    mov     x0, x1                  /* 返回 next（供调用者使用） */
    br      x9                      /* 跳转到 next 的恢复点 */
```

## 4.5 调度器时钟中断处理

```c
/*
 * scheduler_tick() - 由 arch_timer_handler() 每10ms调用一次
 *
 * 参考 kernel/sched/core.c : scheduler_tick()
 *
 * 1. 更新当前进程的 vruntime
 * 2. 检查是否需要抢占：
 *    如果当前进程的 vruntime 超过最小 vruntime 一个调度延迟份额，
 *    则设置 TIF_NEED_RESCHED 标志
 * 3. scheduler_tick 返回后，在中断返回路径（kernel_exit 之前）
 *    检查 TIF_NEED_RESCHED 并调用 schedule()
 *
 * Phase 4 简化：直接在 scheduler_tick 末尾调用 schedule()
 * （不修改 entry.S 的中断返回路径），利用中断上下文完成切换。
 */
#define SCHED_LATENCY_NS    6000000ULL  /* 6ms：调度延迟目标 */
#define SCHED_MIN_GRAN_NS   750000ULL   /* 0.75ms：最小运行粒度 */

void scheduler_tick(void) {
    struct rq *rq = this_rq();
    struct cfs_rq *cfs_rq = &rq->cfs;

    update_curr(cfs_rq);

    if (cfs_rq->nr_running > 1) {
        struct sched_entity *curr_se = &rq->curr->se;
        uint64_t ideal_runtime = SCHED_LATENCY_NS / cfs_rq->nr_running;
        if (ideal_runtime < SCHED_MIN_GRAN_NS)
            ideal_runtime = SCHED_MIN_GRAN_NS;

        uint64_t delta = curr_se->sum_exec_runtime - curr_se->exec_start;
        /* 简化：直接比较本轮运行时间是否超过份额 */
        if (curr_se->vruntime > cfs_rq->min_vruntime + ideal_runtime)
            rq->curr->flags |= TIF_NEED_RESCHED;
    }
}

/*
 * schedule() - 主调度函数
 *
 * Phase 4 调用时机：
 *   1. scheduler_tick() 设置 NEED_RESCHED 后
 *   2. 在 arch_timer_handler() 末尾检查并调用
 */
void schedule(void) {
    struct rq *rq = this_rq();
    struct task_struct *prev = rq->curr;
    struct task_struct *next;

    /* 更新当前进程 vruntime */
    update_curr(&rq->cfs);

    /* 将 prev 放回运行队列（如果仍可运行） */
    if (prev->state == TASK_RUNNING && prev != rq->idle)
        enqueue_entity(&rq->cfs, &prev->se);

    /* 选择下一个进程 */
    next = pick_next_task(rq);

    prev->flags &= ~TIF_NEED_RESCHED;

    if (next != prev) {
        rq->curr = next;
        rq->nr_switches++;
        context_switch(prev, next);  /* 调用 cpu_switch_to */
    }
}
```

## 4.6 验证方法

```c
/*
 * 创建3个优先级不同的内核线程，验证CFS调度。
 *
 * 每个线程执行简单的计数循环，运行一段时间后，
 * 由 start_kernel() 统计各线程的 sum_exec_runtime。
 *
 * 线程A: nice=0,  权重1024
 * 线程B: nice=5,  权重335
 * 线程C: nice=-5, 权重3121
 *
 * 理论CPU时间比例: A:B:C ≈ 1024:335:3121 ≈ 23%:7.5%:70%
 *
 * 验证输出示例：
 *   [sched] Thread A (nice=0):  runtime=230ms  (23%)
 *   [sched] Thread B (nice=5):  runtime=75ms   (7.5%)
 *   [sched] Thread C (nice=-5): runtime=695ms  (70%)
 *   [sched] CFS proportional scheduling: PASS
 *
 * 允许 ±5% 的偏差（调度粒度和定时器精度导致）。
 */
void test_scheduler(void);
```

## 4.7 本阶段产出文件

```
arm64os/
├── include/linux/
│   ├── sched.h                      ← 进程控制块、调度实体、运行队列声明
│   └── rbtree.h                     ← 红黑树接口（rb_node, rb_root, rb_root_cached）
├── lib/
│   └── rbtree.c                     ← 红黑树实现（insert_color, erase, 旋转等）
├── kernel/sched/
│   ├── core.c                       ← 调度器核心（schedule, context_switch, scheduler_tick）
│   └── fair.c                       ← CFS实现（update_curr, enqueue/dequeue, pick_next）
├── arch/arm64/kernel/
│   └── process.S                    ← cpu_switch_to 汇编（注意：.S 不是 .c）
└── drivers/timer/
    └── arm_arch_timer.c             ← 修改：在 handler 中调用 scheduler_tick + schedule
```

### 与原设计的变更说明

1. **删除 `run_queue.c`**：运行队列管理逻辑自然属于 `fair.c`，单独文件无必要
2. **`process.c` → `process.S`**：cpu_switch_to 是纯汇编，应使用 .S 扩展名
3. **新增 `include/linux/rbtree.h` + `lib/rbtree.c`**：CFS 依赖红黑树，原设计遗漏
4. **新增 `include/linux/sched.h`**：数据结构需要头文件声明，供多个 .c 文件共享
5. **`struct rq` 补充**：原设计 scheduler_tick() 引用了 `struct rq` 但未定义
6. **Phase 4 不切换页表**：所有任务为内核线程（mm=NULL），cpu_switch_to 简化
