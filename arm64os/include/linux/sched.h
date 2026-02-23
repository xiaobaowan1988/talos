/* SPDX-License-Identifier: GPL-2.0 */
/*
 * arm64os/include/linux/sched.h
 *
 * 进程控制块与调度器数据结构
 *
 * 参考：include/linux/sched.h
 *       kernel/sched/sched.h
 *
 * Phase 4 实现 CFS 完全公平调度器所需的核心结构体：
 *   - struct task_struct：进程控制块
 *   - struct sched_entity：CFS 调度实体
 *   - struct cfs_rq：CFS 运行队列
 *   - struct rq：每 CPU 运行队列
 *   - struct cpu_context：上下文切换寄存器保存区
 */

#ifndef __LINUX_SCHED_H
#define __LINUX_SCHED_H

#include <linux/types.h>
#include <linux/rbtree.h>

/*
 * ============================================================
 * 进程状态
 * ============================================================
 */
#define TASK_RUNNING        0       /* 正在运行 或 在运行队列中等待 */
#define TASK_DEAD           4       /* 已退出，等待回收 */

/*
 * ============================================================
 * 调度标志位（存放在 task_struct.flags 中）
 * ============================================================
 */
#define TIF_NEED_RESCHED    (1UL << 0)  /* 需要重新调度 */

/*
 * ============================================================
 * CFS 调度参数
 * ============================================================
 */
#define NICE_0_WEIGHT       1024UL      /* nice=0 的标准权重 */
#define SCHED_LATENCY_NS    6000000ULL  /* 6ms：CFS 调度延迟目标 */
#define SCHED_MIN_GRAN_NS   750000ULL   /* 0.75ms：最小运行粒度 */
#define MAX_TASKS           16          /* Phase 4 最大进程数 */

/*
 * ============================================================
 * 权重结构体
 * ============================================================
 */
struct load_weight {
    unsigned long weight;
};

/*
 * ============================================================
 * cpu_context - 上下文切换时保存的 callee-saved 寄存器
 *
 * 布局必须与 arch/arm64/kernel/process.S 中 cpu_switch_to 严格对齐：
 *   offset  0: x19
 *   offset  8: x20
 *   offset 16: x21
 *   offset 24: x22
 *   offset 32: x23
 *   offset 40: x24
 *   offset 48: x25
 *   offset 56: x26
 *   offset 64: x27
 *   offset 72: x28
 *   offset 80: fp (x29)
 *   offset 88: sp
 *   offset 96: pc (返回地址，即 LR)
 *
 * 参考：arch/arm64/include/asm/processor.h struct cpu_context
 * ============================================================
 */
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
    unsigned long fp;       /* x29 */
    unsigned long sp;
    unsigned long pc;       /* 返回地址 */
};

/*
 * ============================================================
 * sched_entity - CFS 调度实体
 *
 * 每个进程包含一个 sched_entity，记录该进程在 CFS 中的调度状态。
 * 通过 run_node 挂入 cfs_rq 的红黑树（按 vruntime 排序）。
 *
 * 参考：include/linux/sched.h struct sched_entity
 * ============================================================
 */
struct sched_entity {
    struct load_weight  load;               /* 权重 */
    struct rb_node      run_node;           /* 红黑树节点（按 vruntime 排序）*/
    unsigned int        on_rq;              /* 1=在运行队列中, 0=不在 */
    uint64_t            vruntime;           /* 虚拟运行时间 */
    uint64_t            exec_start;         /* 本次开始执行的时间戳（ns）*/
    uint64_t            sum_exec_runtime;   /* 累计实际运行时间（ns）*/
};

/*
 * ============================================================
 * task_struct - 进程控制块
 *
 * Phase 4 简化版：
 *   - 所有进程为内核线程（无 mm_struct）
 *   - 静态分配（最多 MAX_TASKS 个）
 *   - 无信号、无文件描述符等
 *
 * 参考：include/linux/sched.h struct task_struct
 * ============================================================
 */
struct task_struct {
    volatile long       state;              /* TASK_RUNNING / TASK_DEAD */
    unsigned long       flags;              /* TIF_NEED_RESCHED 等 */
    void               *stack;              /* 内核栈底地址（低地址端）*/
    int                 pid;                /* 进程 ID */
    int                 prio;               /* 优先级 = nice + 20 (0-39) */
    char                comm[16];           /* 进程名 */
    struct sched_entity se;                 /* CFS 调度实体 */
    struct cpu_context  thread;             /* 上下文切换保存区 */
};

/*
 * THREAD_CPU_CONTEXT - task_struct 中 thread 字段的偏移量
 *
 * 供 process.S 中 cpu_switch_to 使用。
 * 必须与 struct task_struct 的实际布局匹配。
 *
 * 计算方式：
 *   state:    8 bytes (long)
 *   flags:    8 bytes (unsigned long)
 *   stack:    8 bytes (pointer)
 *   pid:      4 bytes (int)
 *   prio:     4 bytes (int)
 *   comm:    16 bytes (char[16])
 *   se:       sched_entity size
 *     load_weight: 8 bytes
 *     rb_node: 24 bytes (3 × unsigned long)
 *     on_rq: 4 bytes + 4 pad
 *     vruntime: 8 bytes
 *     exec_start: 8 bytes
 *     sum_exec_runtime: 8 bytes
 *   se total = 8 + 24 + 8 + 8 + 8 + 8 = 64 bytes
 *
 *   offset = 8 + 8 + 8 + 4 + 4 + 16 + 64 = 112
 */
#define THREAD_CPU_CONTEXT  112

/*
 * ============================================================
 * cfs_rq - CFS 运行队列
 *
 * 参考：kernel/sched/sched.h struct cfs_rq
 * ============================================================
 */
struct cfs_rq {
    struct load_weight      load;           /* 队列总权重 */
    unsigned int            nr_running;     /* 可运行进程数 */
    uint64_t                min_vruntime;   /* 最小 vruntime（单调递增）*/
    struct sched_entity    *curr;           /* 当前正在运行的调度实体 */
    struct rb_root_cached   tasks_timeline; /* 红黑树（按 vruntime 排序）*/
};

/*
 * ============================================================
 * rq - 每 CPU 运行队列
 *
 * Phase 4 单 CPU，只有一个全局 rq。
 *
 * 参考：kernel/sched/sched.h struct rq
 * ============================================================
 */
struct rq {
    struct task_struct     *curr;           /* 当前运行的进程 */
    struct task_struct     *idle;           /* idle 进程 */
    struct cfs_rq           cfs;            /* CFS 运行队列 */
    unsigned int            nr_switches;    /* 上下文切换计数（调试）*/
};

/*
 * ============================================================
 * 外部接口（kernel/sched/core.c 提供）
 * ============================================================
 */

/* 调度器初始化 */
void sched_init(void);

/* 主调度函数：选择下一个进程并切换 */
void schedule(void);

/* 时钟中断回调：更新 vruntime，判断是否需要抢占 */
void scheduler_tick(void);

/* 创建内核线程 */
struct task_struct *kernel_thread_create(void (*fn)(void), const char *name,
                                        int nice);

/* 获取当前运行队列 */
struct rq *this_rq(void);

/* 获取当前进程 */
struct task_struct *get_current(void);

/* 调度器测试 */
void test_scheduler(void);

/*
 * ============================================================
 * 外部接口（kernel/sched/fair.c 提供）
 * ============================================================
 */

/* 将调度实体加入 CFS 运行队列 */
void enqueue_entity(struct cfs_rq *cfs_rq, struct sched_entity *se);

/* 将调度实体从 CFS 运行队列移除 */
void dequeue_entity(struct cfs_rq *cfs_rq, struct sched_entity *se);

/* 更新当前进程的 vruntime */
void update_curr(struct cfs_rq *cfs_rq);

/* 选择 CFS 中 vruntime 最小的调度实体 */
struct sched_entity *pick_next_entity(struct cfs_rq *cfs_rq);

/* 更新 min_vruntime */
void update_min_vruntime(struct cfs_rq *cfs_rq);

#endif /* __LINUX_SCHED_H */
