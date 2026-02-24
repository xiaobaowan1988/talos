# Phase 4 Walkthrough: CFS 完全公平调度器

> **目标**：实现 Linux CFS 调度器，让多个内核线程按权重公平共享 CPU。
> **最终效果**：3 个不同 nice 值的线程按权重比例获得 CPU 时间（±5% 误差）。

---

## 4.1 调度器要解决什么问题？

Phase 3 给了我们 10ms 一次的定时器中断。现在要利用它来切换"谁在运行"。

CFS（Completely Fair Scheduler）的核心思想：**让每个进程获得的 CPU 时间与其权重成正比**。它通过 **虚拟运行时间**（vruntime）追踪公平性 — vruntime 最小的进程最"饥饿"，优先调度。

---

## 4.2 数据结构设计

### task_struct — 进程控制块

```c
struct task_struct {
    unsigned long state;          /* TASK_RUNNING, TASK_DEAD, ... */
    unsigned long flags;          /* TIF_NEED_RESCHED 等 */
    void *stack;                  /* 内核栈虚拟地址 */
    int pid;
    int prio;                     /* nice 值映射的优先级 (0-39) */
    struct mm_struct *mm;         /* 用户地址空间（Phase 5） */
    struct sched_entity se;       /* CFS 调度实体 */
    struct cpu_context thread;    /* 上下文切换保存的寄存器 */
    char comm[16];                /* 进程名 */
};
```

### sched_entity — CFS 调度实体

```c
struct sched_entity {
    struct load_weight load;      /* 权重（由 nice 值决定） */
    uint64_t vruntime;            /* 虚拟运行时间（核心！） */
    uint64_t exec_start;          /* 当前时间片开始时间 */
    uint64_t sum_exec_runtime;    /* 总实际运行时间 */
    int on_rq;                    /* 是否在运行队列中 */
    struct rb_node run_node;      /* 红黑树节点 */
};
```

### cfs_rq — CFS 运行队列

```c
struct cfs_rq {
    struct rb_root_cached tasks_timeline;  /* 红黑树（按 vruntime 排序） */
    uint64_t min_vruntime;                 /* 全局最小 vruntime（单调递增） */
    unsigned long nr_running;              /* 队列中的进程数 */
    struct sched_entity *curr;             /* 当前运行的实体 */
};
```

---

## 4.3 vruntime — 公平性的度量

### 核心公式

```
vruntime += delta_exec × (NICE_0_WEIGHT / weight)
```

- `delta_exec`：实际运行的纳秒数
- `NICE_0_WEIGHT = 1024`：nice 0 的基准权重
- `weight`：当前进程的权重

**效果**：权重高的进程 vruntime 增长慢 → 更容易成为最小值 → 获得更多调度机会。

### nice 值到权重的映射表

```c
static const int sched_prio_to_weight[40] = {
 /* nice -20 */ 88761, 71755, 56483, 46273, 36291,
 /* nice -15 */ 29154, 23254, 18705, 14949, 11916,
 /* nice -10 */  9548,  7620,  6100,  4904,  3906,
 /* nice  -5 */  3121,  2501,  1991,  1586,  1277,
 /* nice   0 */  1024,   820,   655,   526,   423,
 /* nice   5 */   335,   272,   215,   172,   137,
 /* nice  10 */   110,    87,    70,    56,    45,
 /* nice  15 */    36,    29,    23,    18,    15,
};
```

**设计原则**：相邻 nice 值的权重比约为 1.25:1。nice -20 的权重是 nice +19 的 ~5917 倍。

### 举例

假设两个进程 A（nice 0, weight 1024）和 B（nice 5, weight 335）：

```
  A 运行 10ms: vruntime += 10ms × (1024/1024) = 10ms
  B 运行 10ms: vruntime += 10ms × (1024/335)  = 30.6ms

  → A 的 vruntime 增长慢，获得更多调度机会
  → CPU 分配比约为 1024:335 ≈ 3:1
```

---

## 4.4 红黑树 — O(log n) 调度

CFS 用红黑树按 vruntime 排序所有就绪进程，**最左节点 = vruntime 最小 = 下一个调度的进程**。

```
           vruntime=30
          /          \
     vruntime=10    vruntime=50
    /    \
 vruntime=5  vruntime=20    ← 最左节点 = 下次调度
   ▲
   │
  rb_first_cached()  → O(1)
```

### 红黑树关键操作

```c
/* 插入：O(log n) */
static void __enqueue_entity(struct cfs_rq *cfs_rq,
                             struct sched_entity *se)
{
    struct rb_node **link = &cfs_rq->tasks_timeline.rb_root.rb_node;
    struct rb_node *parent = NULL;
    int leftmost = 1;

    while (*link) {
        parent = *link;
        struct sched_entity *entry =
            container_of(parent, struct sched_entity, run_node);

        if (se->vruntime < entry->vruntime) {
            link = &parent->rb_left;
        } else {
            link = &parent->rb_right;
            leftmost = 0;  /* 不再是最左 */
        }
    }

    rb_link_node(&se->run_node, parent, link);
    rb_insert_color_cached(&se->run_node,
                           &cfs_rq->tasks_timeline, leftmost);
}

/* 选择下一个：O(1)（缓存了最左节点） */
static struct sched_entity *pick_next_entity(struct cfs_rq *cfs_rq)
{
    struct rb_node *left = rb_first_cached(&cfs_rq->tasks_timeline);
    if (!left)
        return NULL;
    return container_of(left, struct sched_entity, run_node);
}
```

---

## 4.5 `update_curr()` — 每次调度和 tick 时更新

```c
static void update_curr(struct cfs_rq *cfs_rq)
{
    struct sched_entity *curr = cfs_rq->curr;
    uint64_t now = sched_clock();  /* 读 CNTVCT_EL0 转纳秒 */
    uint64_t delta_exec;

    delta_exec = now - curr->exec_start;
    curr->exec_start = now;
    curr->sum_exec_runtime += delta_exec;

    /* 加权 vruntime */
    curr->vruntime += calc_delta_fair(delta_exec, &curr->load);

    /* 更新全局 min_vruntime（只增不减） */
    update_min_vruntime(cfs_rq);
}
```

### sched_clock() — 纳秒级时钟源

```c
static uint64_t sched_clock(void)
{
    uint64_t cnt, freq;
    __asm__ volatile("mrs %0, cntvct_el0" : "=r"(cnt));
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(freq));
    return (cnt * 1000000000UL) / freq;  /* 转换为纳秒 */
}
```

---

## 4.6 上下文切换 — `cpu_switch_to()`

当调度器决定切换进程时，需要保存当前进程的寄存器，恢复下一个进程的寄存器。

### ARM64 寄存器约定

| 寄存器 | 用途 | 调用约定 |
|--------|------|----------|
| x0-x7 | 参数/返回值 | Caller-saved（调用者保存） |
| x8 | 系统调用号 | Caller-saved |
| x9-x15 | 临时寄存器 | Caller-saved |
| **x19-x28** | **通用** | **Callee-saved（被调用者保存）** |
| x29 (FP) | 帧指针 | Callee-saved |
| x30 (LR) | 返回地址 | 特殊 |

上下文切换只需保存 **callee-saved 寄存器**（x19-x28, FP, SP, LR），因为 caller-saved 寄存器已经由 C 编译器生成的代码保存了。

### cpu_context 结构

```c
struct cpu_context {
    unsigned long x19, x20, x21, x22, x23;
    unsigned long x24, x25, x26, x27, x28;
    unsigned long fp;    /* x29 */
    unsigned long sp;    /* 内核栈指针 */
    unsigned long pc;    /* 恢复执行的地址 */
};
```

### 汇编实现

```asm
/* cpu_switch_to(prev_task, next_task) */
cpu_switch_to:
    /* 保存 prev 的寄存器到 prev->thread */
    stp     x19, x20, [x0, #0]      /* prev->thread.x19, x20 */
    stp     x21, x22, [x0, #16]
    stp     x23, x24, [x0, #32]
    stp     x25, x26, [x0, #48]
    stp     x27, x28, [x0, #64]
    stp     x29, x9,  [x0, #80]     /* x9 = sp (below) */
    mov     x9, sp
    str     x9,  [x0, #88]          /* prev->thread.sp */
    str     x30, [x0, #96]          /* prev->thread.pc = LR */

    /* 恢复 next 的寄存器 */
    ldp     x19, x20, [x1, #0]
    ldp     x21, x22, [x1, #16]
    ldp     x23, x24, [x1, #32]
    ldp     x25, x26, [x1, #48]
    ldp     x27, x28, [x1, #64]
    ldp     x29, x9,  [x1, #80]     /* FP, _ */
    ldr     x9,  [x1, #88]          /* next->thread.sp */
    mov     sp,  x9
    ldr     x9,  [x1, #96]          /* next->thread.pc */

    br      x9                       /* 跳转到 next 的恢复点 */
```

**关键理解**：`br x9` 不是 `ret`。对于**新创建的线程**，`pc` 设置为 `ret_from_kernel_thread`（一个特殊入口）。对于被切换走再切换回的线程，`pc` 就是 `cpu_switch_to` 的返回地址（`x30`），效果等同于 `ret`。

---

## 4.7 调度器 tick 与抢占

```c
void scheduler_tick(void)
{
    struct cfs_rq *cfs_rq = &runqueue.cfs;
    struct sched_entity *curr = cfs_rq->curr;

    update_curr(cfs_rq);

    /* 检查是否应该抢占 */
    if (cfs_rq->nr_running > 1) {
        uint64_t ideal_runtime = SCHED_LATENCY_NS / cfs_rq->nr_running;
        if (ideal_runtime < SCHED_MIN_GRAN_NS)
            ideal_runtime = SCHED_MIN_GRAN_NS;

        if (curr->sum_exec_runtime - curr->prev_sum_exec_runtime
            >= ideal_runtime) {
            /* 设置 "需要调度" 标志 */
            current_task->flags |= TIF_NEED_RESCHED;
        }
    }
}
```

- `SCHED_LATENCY_NS = 6ms`：所有就绪进程在此时间内都应运行一次
- `SCHED_MIN_GRAN_NS = 0.75ms`：最小时间片（避免切换过于频繁）
- 理想时间片 = 6ms / nr_running

---

## 4.8 `schedule()` — 核心调度函数

```c
void schedule(void)
{
    struct rq *rq = &runqueue;
    struct task_struct *prev = rq->curr;
    struct task_struct *next;

    /* 更新当前进程的 vruntime */
    update_curr(&rq->cfs);

    /* 把 prev 放回红黑树（如果还能运行） */
    if (prev->state == TASK_RUNNING && prev != rq->idle)
        enqueue_entity(&rq->cfs, &prev->se);

    /* 从红黑树选最左节点 */
    struct sched_entity *se = pick_next_entity(&rq->cfs);

    if (se) {
        next = container_of(se, struct task_struct, se);
        dequeue_entity(&rq->cfs, se);
    } else {
        next = rq->idle;  /* 没有就绪进程，运行 idle */
    }

    if (prev != next) {
        rq->curr = next;
        rq->cfs.curr = &next->se;
        next->se.exec_start = sched_clock();
        context_switch(prev, next);
    }
}
```

---

## 4.9 创建内核线程

```c
struct task_struct *kernel_thread_create(void (*fn)(void),
                                         const char *name, int nice)
{
    struct task_struct *tsk = alloc_task();

    /* 分配内核栈 (2 pages = 8KB) */
    struct page *stack_page = alloc_pages(1);
    tsk->stack = page_address(stack_page);
    unsigned long stack_top = (unsigned long)tsk->stack + 2 * PAGE_SIZE;

    /* 设置上下文：首次调度时执行 fn */
    tsk->thread.sp = stack_top;
    tsk->thread.pc = (unsigned long)ret_from_kernel_thread;
    tsk->thread.x19 = (unsigned long)fn;  /* 函数指针 */

    /* 设置优先级 */
    tsk->prio = nice + 20;  /* nice → prio (0-39) */
    tsk->se.load.weight = sched_prio_to_weight[tsk->prio];

    /* 加入运行队列 */
    tsk->se.vruntime = runqueue.cfs.min_vruntime;  /* 公平起点 */
    enqueue_entity(&runqueue.cfs, &tsk->se);

    return tsk;
}
```

新线程首次调度：`cpu_switch_to` → `br x9`（x9 = ret_from_kernel_thread）→ `blr x19`（调用 fn）。

---

## 4.10 测试：验证 CFS 公平性

```c
void test_scheduler(void)
{
    /* 创建 3 个线程，不同 nice 值 */
    struct task_struct *t1 = kernel_thread_create(worker, "worker-1", 0);
    struct task_struct *t2 = kernel_thread_create(worker, "worker-2", 0);
    struct task_struct *t3 = kernel_thread_create(worker, "worker-3", 5);

    /* worker 函数：忙等消耗 CPU */

    /* 运行一段时间后比较 sum_exec_runtime */
    /* t1:t2 应约 1:1（相同权重） */
    /* t1:t3 应约 1024:335 ≈ 3:1（权重比） */
    /* 容差 ±5% */
}
```

---

## 4.11 完整调度流程图

```
  Timer IRQ (每 10ms)
      │
      ▼
  scheduler_tick()
      │
      ├─ update_curr()        更新 vruntime
      │
      └─ check preemption     if (运行太久)
           │                    set TIF_NEED_RESCHED
           ▼
  schedule() (在返回用户态前或主动调用)
      │
      ├─ update_curr()        最终更新
      ├─ enqueue prev         放回红黑树
      ├─ pick_next_entity()   选最左节点 (O(1))
      ├─ dequeue next         从树中移除
      └─ context_switch()
           │
           ▼
      cpu_switch_to(prev, next)
           │
           ├─ 保存 prev: x19-x28, FP, SP, LR
           └─ 恢复 next: x19-x28, FP, SP, PC
                │
                ▼
           next 继续执行（从上次被切走的地方）
```

---

## 4.12 Phase 4 核心概念总结

| 概念 | 说明 |
|------|------|
| **vruntime** | 加权虚拟运行时间，越小越优先 |
| **CFS 红黑树** | 按 vruntime 排序，最左 = 下一个 |
| **nice → weight** | 40 级权重表，相邻比 1.25:1 |
| **cpu_switch_to** | 保存/恢复 callee-saved 寄存器 |
| **scheduler_tick** | 每 10ms 更新 vruntime，检查抢占 |
| **SCHED_LATENCY** | 6ms 调度延迟目标 |
| **idle 进程** | 没有就绪进程时运行，PID=-1 |

**Phase 4 奠定的基础**：有了调度器，Phase 5 可以创建用户进程并调度运行。
