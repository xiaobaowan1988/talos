/* SPDX-License-Identifier: GPL-2.0 */
/*
 * arm64os/include/linux/cgroup.h
 *
 * cgroup v2 数据结构与接口
 *
 * 参考：include/linux/cgroup.h
 *       include/linux/cgroup-defs.h
 *       include/linux/memcontrol.h
 *
 * Phase 10 实现 cgroup v2 统一层级（unified hierarchy）：
 *   - 单一层级树，所有控制器挂在同一棵树上
 *   - CPU 控制器：cpu.max（配额）、cpu.weight（权重）
 *   - Memory 控制器：memory.max（上限）、memory.current（当前使用）
 *   - PIDs 控制器：pids.max（最大进程数）、pids.current（当前进程数）
 *
 * 简化说明：
 *   - 使用静态池分配
 *   - 控制器状态直接嵌入 cgroup 结构体（不使用 cgroup_subsys_state 间接层）
 *   - 无 kernfs 文件系统接口（通过 C API 直接操作）
 */

#ifndef __LINUX_CGROUP_H
#define __LINUX_CGROUP_H

#include <linux/types.h>
#include <linux/list.h>

/* ---- 控制器 ID ---- */
#define CGROUP_SUBSYS_COUNT     3
#define CGROUP_CPU_ID           0
#define CGROUP_MEM_ID           1
#define CGROUP_PIDS_ID          2

/* ---- 静态池大小 ---- */
#define MAX_CGROUPS             8
#define MAX_CSS_SETS            16

/* ---- 特殊值 ---- */
#define CGROUP_MAX_UNLIMITED    ((unsigned long)-1)

/*
 * ============================================================
 * cgroup_subsys_state — 每个 cgroup 子系统的状态
 *
 * 参考：include/linux/cgroup-defs.h struct cgroup_subsys_state
 *
 * 控制器通过 css 与 cgroup 关联。
 * 每个控制器的具体状态结构体（如 mem_cgroup）嵌入一个 css。
 * ============================================================
 */
struct cgroup;  /* 前向声明 */

struct cgroup_subsys_state {
    struct cgroup              *cgroup;     /* 所属 cgroup */
    struct cgroup_subsys_state *parent;     /* 父 cgroup 中对应的 css */
    int                         refcnt;     /* 引用计数 */
};

/*
 * ============================================================
 * cgroup — cgroup 节点
 *
 * 参考：include/linux/cgroup-defs.h struct cgroup
 *
 * cgroup v2 使用单一层级树。根 cgroup 代表系统全局资源。
 * 子 cgroup 为容器或进程组设定资源限制。
 * ============================================================
 */
#define CGROUP_NAME_LEN         32
#define CGROUP_MAX_CHILDREN     4

struct cgroup {
    /* 树结构 */
    struct cgroup              *parent;
    struct cgroup              *children[CGROUP_MAX_CHILDREN];
    int                         nr_children;
    int                         id;
    char                        name[CGROUP_NAME_LEN];

    /* 控制器子系统状态 */
    struct cgroup_subsys_state *subsys[CGROUP_SUBSYS_COUNT];

    /* 进程计数（不递归，仅当前 cgroup 直接成员） */
    int                         nr_procs;

    /* CPU 控制器参数 */
    unsigned long               cpu_max_quota;   /* 微秒/周期，0=无限 */
    unsigned long               cpu_max_period;  /* 周期（微秒），默认 100000 */
    unsigned long               cpu_weight;      /* 权重 1-10000，默认 100 */

    /* Memory 控制器参数 */
    unsigned long               memory_max;      /* 字节，ULONG_MAX=无限 */
    unsigned long               memory_current;  /* 当前使用量（字节）*/

    /* PIDs 控制器参数 */
    int                         pids_max;        /* 最大进程数，-1=无限 */
    int                         pids_current;    /* 当前进程数 */
};

/*
 * ============================================================
 * css_set — 每进程 cgroup 子系统状态集合
 *
 * 参考：include/linux/cgroup-defs.h struct css_set
 *
 * 每个进程持有一个 css_set，指向其所在 cgroup。
 * task_struct->cgroups 指向 css_set。
 * ============================================================
 */
struct css_set {
    struct cgroup              *cgrp;       /* 所在 cgroup */
    int                         refcount;   /* 引用计数 */
};

/*
 * ============================================================
 * 外部接口
 * ============================================================
 */

/* 根 cgroup 和初始 css_set */
extern struct cgroup        root_cgroup;
extern struct css_set       init_css_set;

/* 前向声明（完整定义在 include/linux/sched.h）*/
struct task_struct;

/* --- cgroup.c (核心) --- */
void cgroup_init(void);
struct cgroup *cgroup_create(struct cgroup *parent, const char *name);
int cgroup_attach_task(struct cgroup *cgrp, struct task_struct *task);
int cgroup_detach_task(struct cgroup *cgrp, struct task_struct *task);
struct cgroup *cgroup_get(struct cgroup *cgrp);

/* --- cpu.c (CPU 控制器) --- */
void cpu_cgroup_init(struct cgroup *cgrp);
void cpu_cgroup_set_max(struct cgroup *cgrp,
                         unsigned long quota_us, unsigned long period_us);
void cpu_cgroup_set_weight(struct cgroup *cgrp, unsigned long weight);

/* --- memory.c (内存控制器) --- */
void mem_cgroup_init(struct cgroup *cgrp);
void mem_cgroup_set_max(struct cgroup *cgrp, unsigned long max_bytes);
int mem_cgroup_charge(struct cgroup *cgrp, unsigned long nr_bytes);
void mem_cgroup_uncharge(struct cgroup *cgrp, unsigned long nr_bytes);
int mem_cgroup_oom_check(struct cgroup *cgrp);

/* --- pids.c (PIDs 控制器) --- */
void pids_cgroup_init(struct cgroup *cgrp);
void pids_cgroup_set_max(struct cgroup *cgrp, int max);
int pids_cgroup_can_fork(struct cgroup *cgrp);
void pids_cgroup_charge(struct cgroup *cgrp);
void pids_cgroup_uncharge(struct cgroup *cgrp);

#endif /* __LINUX_CGROUP_H */
