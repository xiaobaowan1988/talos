# Phase 4：CFS 调度器

## 知识来源总览

- **Linux CFS 设计文档**：约 35%（vruntime、红黑树、权重表）
- **ARMv8-A AAPCS64 调用约定**：约 25%（callee-saved 寄存器、上下文切换）
- **ARM64 汇编**：约 20%（cpu_switch_to 实现）
- **Phase 4 文档**：约 20%

## task_struct 进程描述符

```c
struct task_struct {
    struct cpu_context  cpu_context;     /* 上下文切换保存点 */
    long                state;           /* TASK_RUNNING / TASK_INTERRUPTIBLE */
    long                counter;         /* 时间片 */
    long                priority;        /* 静态优先级 */
    long                pid;
    unsigned long       flags;
    struct list_head    run_list;        /* 运行队列链表节点 */

    /* CFS 相关 */
    u64                 vruntime;        /* 虚拟运行时间 */
    unsigned long       weight;          /* 权重（由 nice 值决定）*/
    struct rb_node      rb_node;         /* 红黑树节点 */
};
```

**vruntime（虚拟运行时间）** 是 CFS 的核心概念。公式：

```
vruntime += actual_runtime × (NICE_0_WEIGHT / task_weight)
```

权重高的任务（高优先级），vruntime 增长慢 → 被调度的机会更多。CFS 每次选择 vruntime 最小的任务运行——"最被亏待的任务优先"。

**weight 权重表**（来源：Linux `kernel/sched/core.c`）：
```
nice  0 → weight 1024 （基准）
nice -1 → weight 1277 （约 1.25 倍）
nice  1 → weight  820 （约 0.8 倍）
nice -20→ weight 88761（最高优先级）
nice  19→ weight   15  （最低优先级）
```

每个 nice 级别的权重差约 1.25 倍（10%的 CPU 时间差异）。

## cpu_context 与上下文切换

```c
struct cpu_context {
    unsigned long x19;
    unsigned long x20;
    unsigned long x21;
    unsigned long x22;
    unsigned long x23;
    unsigned long x24;
    unsigned long x25;
    unsigned long x26;
    unsigned long x27;
    unsigned long x28;
    unsigned long fp;   /* x29 */
    unsigned long sp;
    unsigned long pc;   /* 恢复后的执行地址 */
};
```

**为什么只保存 x19-x28 + fp + sp + pc？**

来源：ARM64 AAPCS64 调用约定。

- x0-x18：caller-saved（调用者保存）。调用 `schedule()` 的函数已经保存了这些寄存器（或者不需要保留它们的值）
- x19-x28：callee-saved（被调用者保存）。`cpu_switch_to` 作为被调用者必须保存
- x29 (fp)：帧指针，callee-saved
- x30 (lr)：返回地址，保存为 pc 字段

### cpu_switch_to 汇编实现

```asm
/* void cpu_switch_to(struct task_struct *prev, struct task_struct *next) */
cpu_switch_to:
    /* 保存 prev 的上下文到 prev->cpu_context */
    mov x10, #THREAD_CPU_CONTEXT
    add x8, x0, x10

    stp x19, x20, [x8], #16
    stp x21, x22, [x8], #16
    stp x23, x24, [x8], #16
    stp x25, x26, [x8], #16
    stp x27, x28, [x8], #16
    stp x29, x30, [x8], #16   /* fp + lr(作为pc) */
    mov x9, sp
    str x9, [x8]               /* sp */

    /* 恢复 next 的上下文 */
    add x8, x1, x10
    ldp x19, x20, [x8], #16
    ldp x21, x22, [x8], #16
    ldp x23, x24, [x8], #16
    ldp x25, x26, [x8], #16
    ldp x27, x28, [x8], #16
    ldp x29, x30, [x8], #16   /* fp + pc→lr */
    ldr x9, [x8]
    mov sp, x9                 /* 恢复栈指针 */

    ret                        /* 跳转到 x30(lr) = next 的 pc */
```

**关键洞察**：`ret` 指令跳转到 x30（LR）。对于 prev 来说，保存时 x30 = `cpu_switch_to` 的返回地址。当 prev 被重新调度时，`ret` 会回到 prev 当初调用 `schedule()` 的地方——就好像 `schedule()` 刚刚返回一样。

**这就是上下文切换的魔法**：进程 A 调用 `schedule()`，在 `cpu_switch_to` 中保存寄存器、恢复进程 B 的寄存器、`ret` → 进程 B 从它当初暂停的地方继续执行。从每个进程的视角看，`schedule()` 就是一个普通的函数调用。

## schedule() 调度函数

```c
void schedule(void) {
    local_irq_disable();

    struct task_struct *prev = current;
    struct task_struct *next = pick_next_task();

    if (next != prev) {
        current = next;
        cpu_switch_to(prev, next);
    }

    local_irq_enable();
}
```

**`local_irq_disable()`**：`msr daifset, #2`（屏蔽 IRQ）。调度过程中不能被中断打断——否则可能出现 prev 的寄存器保存了一半就被抢占。

**`pick_next_task()`**：从红黑树中取 vruntime 最小的节点，O(1)（最左节点有缓存指针）。

## scheduler_tick 与 need_resched

```c
void scheduler_tick(void) {
    struct task_struct *curr = current;
    u64 now = timer_get_ticks();
    u64 delta = now - curr->last_tick;

    /* 更新 vruntime */
    curr->vruntime += calc_delta(delta, curr->weight);
    curr->last_tick = now;

    /* 检查是否需要抢占 */
    struct task_struct *next = pick_next_task();
    if (next->vruntime < curr->vruntime)
        set_need_resched(curr);
}
```

`scheduler_tick` 在 Phase 3 的 Timer 中断处理程序中调用。每个 tick 更新当前进程的 vruntime，如果红黑树中有 vruntime 更小的进程，设置 `need_resched` 标志。

**实际的调度切换不在 timer 中断中发生**——中断处理程序中做上下文切换很危险（中断栈可能不够用）。`need_resched` 标志在 `kernel_exit` 宏中检查：

```asm
kernel_exit:
    /* 检查 need_resched */
    ldr x0, [current, #NEED_RESCHED_OFFSET]
    cbnz x0, call_schedule    /* 非零 → 调用 schedule() */
    /* 恢复寄存器，返回被中断的代码 */
    ...
```
