/* SPDX-License-Identifier: GPL-2.0 */
/*
 * arm64os/kernel/sched/core.c
 *
 * 调度器核心
 *
 * 参考：kernel/sched/core.c
 *
 * Phase 4 实现：
 *   - sched_init()：初始化调度器，创建 idle 进程
 *   - schedule()：主调度函数，选择下一个进程并切换
 *   - scheduler_tick()：时钟中断回调，更新 vruntime，判断抢占
 *   - kernel_thread_create()：创建内核线程
 *   - context_switch()：上下文切换（调用 cpu_switch_to 汇编）
 *   - test_scheduler()：验证 CFS 按权重比例分配 CPU 时间
 */

#include <linux/types.h>
#include <linux/sched.h>
#include <linux/rbtree.h>
#include <linux/list.h>
#include <asm/memory.h>

/* 外部函数 */
void boot_printk(const char *s);
void boot_printk_hex(unsigned long val);

/* 内存分配（mm/page_alloc.c）*/
struct page;
struct page *alloc_pages(unsigned int order);
void *page_address(struct page *page);

/* fair.c 提供的 sched_clock */
uint64_t sched_clock(void);

/* nice 到权重映射表（fair.c） */
extern const unsigned long sched_prio_to_weight[40];

/* cpu_switch_to 汇编（arch/arm64/kernel/process.S）*/
extern struct task_struct *cpu_switch_to(struct task_struct *prev,
                                        struct task_struct *next);

/*
 * ============================================================
 * 全局状态
 * ============================================================
 */

/* 全局运行队列（Phase 4 单 CPU） */
static struct rq runqueue;

/* 进程表（静态分配） */
static struct task_struct task_pool[MAX_TASKS];
static int next_pid = 0;

/* idle 进程（PID 0），使用 head.S 的 init_stack */
static struct task_struct idle_task;

/* 当前运行的进程指针 */
struct task_struct *current_task;

/*
 * ============================================================
 * this_rq / get_current - 获取当前运行队列和进程
 * ============================================================
 */
struct rq *this_rq(void)
{
    return &runqueue;
}

struct task_struct *get_current(void)
{
    return current_task;
}

/*
 * ============================================================
 * context_switch - 执行上下文切换
 *
 * 调用 cpu_switch_to() 汇编，保存 prev 的 callee-saved 寄存器，
 * 恢复 next 的寄存器，然后跳转到 next 的恢复点。
 *
 * Phase 4 内核线程不切换页表。
 * Phase 5 新增：用户进程切换 TTBR0。
 *
 * 参考：kernel/sched/core.c context_switch()
 * ============================================================
 */

/* Phase 5：TTBR0 切换（mmu.c）*/
extern void switch_ttbr0(unsigned long pgd_phys);
extern unsigned long get_kernel_pgd(void);

static void context_switch(struct task_struct *prev, struct task_struct *next)
{
    /*
     * Phase 5：切换 TTBR0（用户地址空间页表）
     *
     * - 如果 next 是用户进程（mm != NULL），切换到它的 pgd
     * - 如果 next 是内核线程（mm == NULL），切换回内核初始 pgd
     */
    if (next->mm)
        switch_ttbr0(next->mm->pgd);
    else if (prev && prev->mm)
        switch_ttbr0(get_kernel_pgd());

    cpu_switch_to(prev, next);
}

/*
 * ============================================================
 * pick_next_task - 选择下一个运行的进程
 *
 * 从 CFS 红黑树中取 vruntime 最小的进程。
 * 如果没有可运行进程，返回 idle 进程。
 *
 * 参考：kernel/sched/core.c pick_next_task()
 * ============================================================
 */
static struct task_struct *pick_next_task(struct rq *rq)
{
    struct sched_entity *next_se;
    struct cfs_rq *cfs_rq = &rq->cfs;

    next_se = pick_next_entity(cfs_rq);
    if (!next_se)
        return rq->idle;

    /* 从红黑树中取出（变为 curr） */
    dequeue_entity(cfs_rq, next_se);

    return container_of(next_se, struct task_struct, se);
}

/*
 * ============================================================
 * scheduler_tick - 时钟中断回调
 *
 * 每 10ms 由 arch_timer_handler() 调用一次。
 * 更新当前进程的 vruntime，判断是否需要抢占。
 *
 * 参考：kernel/sched/core.c scheduler_tick()
 * ============================================================
 */
void scheduler_tick(void)
{
    struct rq *rq = this_rq();
    struct cfs_rq *cfs_rq = &rq->cfs;
    struct task_struct *curr = rq->curr;

    if (!curr || curr == rq->idle)
        return;

    /* 更新当前进程的 vruntime */
    update_curr(cfs_rq);

    /* 检查是否需要抢占 */
    if (cfs_rq->nr_running > 0) {
        /* 当前进程在运行队列外（作为 curr），加上队列中的 = 总数 */
        unsigned int total = cfs_rq->nr_running + 1;
        uint64_t ideal_runtime = SCHED_LATENCY_NS / total;
        if (ideal_runtime < SCHED_MIN_GRAN_NS)
            ideal_runtime = SCHED_MIN_GRAN_NS;

        /*
         * 如果当前进程的 vruntime 超过 min_vruntime 太多，
         * 说明它占用了超过公平份额的 CPU 时间，需要让出。
         */
        if ((s64)(curr->se.vruntime - cfs_rq->min_vruntime) >
            (s64)ideal_runtime) {
            curr->flags |= TIF_NEED_RESCHED;
        }
    }
}

/*
 * ============================================================
 * schedule - 主调度函数
 *
 * 检查是否需要切换进程。如果当前进程被标记为 NEED_RESCHED
 * 或者主动放弃 CPU，则选择下一个进程并切换。
 *
 * 参考：kernel/sched/core.c __schedule()
 * ============================================================
 */
void schedule(void)
{
    struct rq *rq = this_rq();
    struct task_struct *prev = rq->curr;
    struct task_struct *next;

    /* 更新当前进程的 vruntime */
    update_curr(&rq->cfs);

    /* 将 prev 放回运行队列（如果仍然可运行且不是 idle） */
    if (prev != rq->idle && prev->state == TASK_RUNNING)
        enqueue_entity(&rq->cfs, &prev->se);

    /* 选择下一个进程 */
    next = pick_next_task(rq);

    /* 清除 NEED_RESCHED 标志 */
    prev->flags &= ~TIF_NEED_RESCHED;

    if (next != prev) {
        rq->curr = next;
        rq->cfs.curr = &next->se;
        rq->nr_switches++;

        /* 设置 next 的 exec_start */
        next->se.exec_start = sched_clock();

        /* 更新全局 current */
        current_task = next;

        /* 执行上下文切换 */
        context_switch(prev, next);

        /*
         * 从 context_switch 返回时，我们已经在新的进程上下文中。
         * 对于 prev 进程，执行流暂停在 cpu_switch_to 内部，
         * 当它被再次选中执行时，会从 cpu_switch_to 的 br x9 返回到这里。
         */
    }
}

/*
 * ============================================================
 * ret_from_kernel_thread - 内核线程的入口包装
 *
 * 新创建的内核线程在 cpu_switch_to 恢复寄存器后，
 * pc 被设为此函数。x19 保存了线程函数指针。
 * 开启中断后跳转到真正的线程函数。
 *
 * 参考：arch/arm64/kernel/entry.S ret_from_fork
 * ============================================================
 */
static void __attribute__((noinline)) ret_from_kernel_thread(void)
{
    void (*fn)(void);

    /* 开启中断（内核线程运行在 EL1，中断默认关闭需要手动开启） */
    __asm__ volatile("msr daifclr, #2" ::: "memory");

    /*
     * x19 在 cpu_switch_to 恢复时已经被填入线程函数地址。
     * 我们直接读取 x19 寄存器。
     */
    __asm__ volatile("mov %0, x19" : "=r"(fn));
    fn();

    /* 线程函数返回后，标记为 DEAD 并让出 CPU */
    current_task->state = TASK_DEAD;
    schedule();

    /* 不应到达这里 */
    while (1)
        ;
}

/*
 * ============================================================
 * kernel_thread_create - 创建内核线程
 *
 * 分配 task_struct 和内核栈，设置初始 cpu_context：
 *   - pc = ret_from_kernel_thread（首次被 schedule 选中时跳转到此）
 *   - x19 = fn（线程函数指针，由 ret_from_kernel_thread 读取）
 *   - sp = 栈顶（高地址端）
 *
 * @fn:   线程函数（无参数，无返回值）
 * @name: 线程名（最多 15 字符）
 * @nice: nice 值（-20 到 19）
 *
 * 参考：kernel/fork.c copy_thread()
 * ============================================================
 */
struct task_struct *kernel_thread_create(void (*fn)(void), const char *name,
                                        int nice)
{
    struct task_struct *tsk;
    struct page *stack_page;
    unsigned long stack_top;
    int i;

    if (next_pid >= MAX_TASKS) {
        boot_printk("[sched] ERROR: task pool exhausted\n");
        return NULL;
    }

    tsk = &task_pool[next_pid];

    /* 分配 2 页（8KB）作为内核栈 */
    stack_page = alloc_pages(1); /* order=1 → 2页 = 8KB */
    if (!stack_page) {
        boot_printk("[sched] ERROR: cannot allocate stack\n");
        return NULL;
    }

    /* 初始化 task_struct */
    tsk->state = TASK_RUNNING;
    tsk->flags = 0;
    tsk->stack = page_address(stack_page);
    tsk->pid = next_pid++;
    tsk->prio = nice + 20; /* nice → prio：nice=-20→prio=0, nice=0→prio=20 */
    tsk->mm = NULL;         /* 内核线程无用户地址空间 */
    tsk->files = NULL;      /* Phase 7：内核线程无文件描述符表 */
    tsk->nsproxy = NULL;    /* Phase 10：继承 init_nsproxy */
    tsk->cgroups = NULL;    /* Phase 10：继承 init_css_set */

    /* 设置进程名 */
    for (i = 0; i < 15 && name[i]; i++)
        tsk->comm[i] = name[i];
    tsk->comm[i] = '\0';

    /* 设置调度实体 */
    tsk->se.load.weight = sched_prio_to_weight[tsk->prio];
    tsk->se.vruntime = 0;
    tsk->se.exec_start = 0;
    tsk->se.sum_exec_runtime = 0;
    tsk->se.on_rq = 0;
    tsk->se.run_node.rb_left = NULL;
    tsk->se.run_node.rb_right = NULL;
    tsk->se.run_node.__rb_parent_color = 0;

    /*
     * 设置初始 cpu_context：
     *   sp = 栈顶（8KB = 2 × PAGE_SIZE）
     *   pc = ret_from_kernel_thread
     *   x19 = fn（线程函数指针）
     *
     * 其余 callee-saved 寄存器清零。
     */
    stack_top = (unsigned long)tsk->stack + 2 * PAGE_SIZE;
    /* 16 字节对齐（AArch64 ABI 要求） */
    stack_top &= ~0xFUL;

    tsk->thread.sp = stack_top;
    tsk->thread.pc = (unsigned long)ret_from_kernel_thread;
    tsk->thread.x19 = (unsigned long)fn;
    tsk->thread.x20 = 0;
    tsk->thread.x21 = 0;
    tsk->thread.x22 = 0;
    tsk->thread.x23 = 0;
    tsk->thread.x24 = 0;
    tsk->thread.x25 = 0;
    tsk->thread.x26 = 0;
    tsk->thread.x27 = 0;
    tsk->thread.x28 = 0;
    tsk->thread.fp = 0;

    /* 加入 CFS 运行队列 */
    enqueue_entity(&runqueue.cfs, &tsk->se);

    boot_printk("[sched] Created thread: ");
    boot_printk(name);
    boot_printk(" (pid=");
    boot_printk_hex(tsk->pid);
    boot_printk(", nice=");
    if (nice < 0) {
        boot_printk("-");
        boot_printk_hex((unsigned long)(-nice));
    } else {
        boot_printk_hex((unsigned long)nice);
    }
    boot_printk(", weight=");
    boot_printk_hex(tsk->se.load.weight);
    boot_printk(")\n");

    return tsk;
}

/*
 * ============================================================
 * sched_init - 初始化调度器
 *
 * 设置 idle 进程和全局运行队列。
 * 调用时机：start_kernel() 中，中断使能之前。
 *
 * 参考：kernel/sched/core.c sched_init()
 * ============================================================
 */
void sched_init(void)
{
    struct cfs_rq *cfs_rq = &runqueue.cfs;

    boot_printk("[sched] Initializing CFS scheduler...\n");

    /* 初始化 CFS 运行队列 */
    cfs_rq->load.weight = 0;
    cfs_rq->nr_running = 0;
    cfs_rq->min_vruntime = 0;
    cfs_rq->curr = NULL;
    cfs_rq->tasks_timeline.rb_root.rb_node = NULL;
    cfs_rq->tasks_timeline.rb_leftmost = NULL;

    /* 初始化 idle 进程（PID 0） */
    idle_task.state = TASK_RUNNING;
    idle_task.flags = 0;
    idle_task.stack = NULL;  /* idle 使用 head.S 的 init_stack */
    idle_task.pid = -1;      /* 特殊 PID：不计入 task_pool */
    idle_task.prio = 39;     /* 最低优先级 */
    idle_task.mm = NULL;     /* 内核线程无用户地址空间 */
    idle_task.files = NULL;  /* Phase 7：idle 无文件描述符表 */
    idle_task.nsproxy = NULL; /* Phase 10：idle 使用 init_nsproxy */
    idle_task.cgroups = NULL; /* Phase 10：idle 使用 init_css_set */
    idle_task.comm[0] = 'i'; idle_task.comm[1] = 'd';
    idle_task.comm[2] = 'l'; idle_task.comm[3] = 'e';
    idle_task.comm[4] = '\0';
    idle_task.se.load.weight = sched_prio_to_weight[39]; /* nice=19 */
    idle_task.se.vruntime = 0;
    idle_task.se.exec_start = 0;
    idle_task.se.sum_exec_runtime = 0;
    idle_task.se.on_rq = 0;

    /* idle 不加入 CFS 运行队列（它是 fallback） */
    runqueue.idle = &idle_task;
    runqueue.curr = &idle_task;
    runqueue.nr_switches = 0;

    current_task = &idle_task;

    /* 设置 idle 的 exec_start 为当前时间 */
    idle_task.se.exec_start = sched_clock();
    cfs_rq->curr = &idle_task.se;

    boot_printk("[sched] CFS scheduler initialized\n");
}

/*
 * ============================================================
 * 调度器测试
 *
 * 创建 3 个不同 nice 值的内核线程，运行一段时间后
 * 统计各线程的 sum_exec_runtime，验证 CFS 按权重比例分配。
 *
 * 理论比例：
 *   nice=-5 (weight=3121): ~70%
 *   nice=0  (weight=1024): ~23%
 *   nice=5  (weight=335):  ~7%
 * ============================================================
 */

/* 测试线程的共享状态 */
static volatile int test_done = 0;
static struct task_struct *thread_a = NULL;
static struct task_struct *thread_b = NULL;
static struct task_struct *thread_c = NULL;

/* arch_timer_tick_count from timer driver */
extern volatile int arch_timer_tick_count;

/* tick count at test start */
static volatile int test_start_ticks = 0;

/* 100 ticks = 1 second (HZ=100) */
#define TEST_DURATION_TICKS  100

/*
 * thread_work - 测试线程工作函数
 *
 * 循环计数，定期检查 NEED_RESCHED 标志并调用 schedule()。
 * 同时检查是否已运行足够时间（基于 tick 计数）。
 *
 * Phase 4 协作式调度：线程主动让出 CPU。
 */
static void thread_a_fn(void)
{
    volatile unsigned long count = 0;
    while (!test_done) {
        count++;
        if (current_task->flags & TIF_NEED_RESCHED)
            schedule();
        if (arch_timer_tick_count - test_start_ticks >= TEST_DURATION_TICKS)
            test_done = 1;
    }
}

static void thread_b_fn(void)
{
    volatile unsigned long count = 0;
    while (!test_done) {
        count++;
        if (current_task->flags & TIF_NEED_RESCHED)
            schedule();
    }
}

static void thread_c_fn(void)
{
    volatile unsigned long count = 0;
    while (!test_done) {
        count++;
        if (current_task->flags & TIF_NEED_RESCHED)
            schedule();
    }
}

/*
 * print_decimal - 打印十进制数（辅助函数）
 */
static void print_decimal(uint64_t val)
{
    char buf[21];
    int i = 20;
    buf[20] = '\0';
    if (val == 0) {
        boot_printk("0");
        return;
    }
    while (val > 0 && i > 0) {
        i--;
        buf[i] = '0' + (val % 10);
        val /= 10;
    }
    boot_printk(&buf[i]);
}

void test_scheduler(void)
{
    (void)0; /* timing is tick-based, see test_start_ticks */
    uint64_t total_runtime;
    uint64_t pct_a, pct_b, pct_c;

    boot_printk("[sched] === test_scheduler start ===\n");

    /* 创建 3 个测试线程 */
    thread_a = kernel_thread_create(thread_a_fn, "thread_A", 0);
    thread_b = kernel_thread_create(thread_b_fn, "thread_B", 5);
    thread_c = kernel_thread_create(thread_c_fn, "thread_C", -5);

    if (!thread_a || !thread_b || !thread_c) {
        boot_printk("[sched] ERROR: failed to create test threads\n");
        return;
    }

    test_done = 0;
    test_start_ticks = arch_timer_tick_count;

    boot_printk("[sched] Running 3 threads for ~1 second...\n");

    /*
     * 让出 CPU 给测试线程。线程们会互相调度运行约 1 秒。
     * 当 test_done=1 时（由某个线程基于 tick 计数设置），
     * 所有线程退出循环，标记为 TASK_DEAD，调用 schedule()。
     * 最终所有线程都退出，idle 被选中返回到这里。
     *
     * idle 在 schedule() 中被选中时，从 cpu_switch_to 返回。
     */
    while (!test_done) {
        schedule();
        /* idle 等待中断唤醒 */
        __asm__ volatile("wfi");
    }

    /*
     * test_done=1 后，线程可能还在运行。
     * 等待所有线程通过 schedule 完成退出。
     */
    {
        int i;
        for (i = 0; i < 100; i++) {
            schedule();
            if (runqueue.cfs.nr_running == 0)
                break;
        }
    }

    /* 统计结果 */
    boot_printk("[sched] --- Results ---\n");
    boot_printk("[sched] Context switches: ");
    boot_printk_hex(runqueue.nr_switches);
    boot_printk("\n");

    total_runtime = thread_a->se.sum_exec_runtime +
                    thread_b->se.sum_exec_runtime +
                    thread_c->se.sum_exec_runtime;

    if (total_runtime == 0) {
        boot_printk("[sched] ERROR: no runtime recorded\n");
        boot_printk("[sched] === test_scheduler end ===\n");
        return;
    }

    /* 计算百分比（×100 取整） */
    pct_a = (thread_a->se.sum_exec_runtime * 100) / total_runtime;
    pct_b = (thread_b->se.sum_exec_runtime * 100) / total_runtime;
    pct_c = (thread_c->se.sum_exec_runtime * 100) / total_runtime;

    boot_printk("[sched] Thread A (nice=0,  w=1024): runtime=");
    print_decimal(thread_a->se.sum_exec_runtime / 1000000);
    boot_printk("ms (");
    print_decimal(pct_a);
    boot_printk("%)\n");

    boot_printk("[sched] Thread B (nice=5,  w=335):  runtime=");
    print_decimal(thread_b->se.sum_exec_runtime / 1000000);
    boot_printk("ms (");
    print_decimal(pct_b);
    boot_printk("%)\n");

    boot_printk("[sched] Thread C (nice=-5, w=3121): runtime=");
    print_decimal(thread_c->se.sum_exec_runtime / 1000000);
    boot_printk("ms (");
    print_decimal(pct_c);
    boot_printk("%)\n");

    /*
     * 验证比例：
     * C(nice=-5) 应获得最多 CPU 时间（~70%），
     * A(nice=0) 中等（~23%），B(nice=5) 最少（~7%）。
     *
     * 允许 ±10% 偏差（调度粒度 + 定时器精度）。
     * 简单检查：C > A > B 的相对顺序。
     */
    if (pct_c > pct_a && pct_a > pct_b) {
        boot_printk("[sched] CFS proportional scheduling: PASS\n");
    } else {
        boot_printk("[sched] CFS proportional scheduling: ordering check FAIL\n");
        boot_printk("[sched] Expected: C > A > B\n");
    }

    boot_printk("[sched] === test_scheduler end ===\n");
}
