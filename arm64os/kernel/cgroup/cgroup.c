/* SPDX-License-Identifier: GPL-2.0 */
/*
 * arm64os/kernel/cgroup/cgroup.c
 *
 * cgroup v2 核心 — 统一层级管理
 *
 * 参考：kernel/cgroup/cgroup.c
 *       kernel/cgroup/cgroup-v2.c
 *
 * Phase 10 实现：
 *   - root_cgroup：根 cgroup（代表系统全局资源）
 *   - init_css_set：初始 css_set（所有进程默认在根 cgroup）
 *   - cgroup_init()：初始化 cgroup 子系统
 *   - cgroup_create()：在指定父 cgroup 下创建子 cgroup
 *   - cgroup_attach_task()：将进程附加到 cgroup
 *   - cgroup_detach_task()：将进程从 cgroup 分离
 *
 * cgroup v2 使用单一层级（unified hierarchy），所有控制器挂在同一棵树上：
 *
 *   /sys/fs/cgroup/               （root_cgroup）
 *   └── container1/               （子 cgroup）
 *       ├── cpu.max               CPU 配额限制
 *       ├── cpu.weight            CPU 权重
 *       ├── memory.max            内存上限
 *       ├── memory.current        当前内存使用（只读）
 *       ├── pids.max              最大进程数
 *       └── pids.current          当前进程数（只读）
 */

#include <linux/types.h>
#include <linux/sched.h>
#include <linux/cgroup.h>
#include <linux/nsproxy.h>

/* 外部函数 */
void boot_printk(const char *s);
void boot_printk_hex(unsigned long val);

/*
 * ============================================================
 * 静态池
 * ============================================================
 */
static struct cgroup cgroup_pool[MAX_CGROUPS];
static int cgroup_pool_idx = 0;

static struct css_set css_set_pool[MAX_CSS_SETS];
static int css_set_pool_idx = 0;

static int next_cgroup_id = 0;

/*
 * ============================================================
 * 根 cgroup 和初始 css_set
 * ============================================================
 */
struct cgroup root_cgroup;
struct css_set init_css_set;

/*
 * ============================================================
 * cgroup_init — 初始化 cgroup v2 子系统
 *
 * 设置根 cgroup 和初始 css_set。
 * 调用各控制器的初始化函数。
 *
 * 参考：kernel/cgroup/cgroup.c cgroup_init()
 * ============================================================
 */
void cgroup_init(void)
{
    int i;

    boot_printk("[cgroup] Initializing cgroup v2...\n");

    /* 初始化根 cgroup */
    root_cgroup.parent = NULL;
    root_cgroup.nr_children = 0;
    root_cgroup.id = next_cgroup_id++;
    root_cgroup.nr_procs = 0;

    /* 设置名称 */
    root_cgroup.name[0] = '/';
    root_cgroup.name[1] = '\0';

    /* 清零子 cgroup 指针 */
    for (i = 0; i < CGROUP_MAX_CHILDREN; i++)
        root_cgroup.children[i] = NULL;

    /* 清零控制器子系统状态 */
    for (i = 0; i < CGROUP_SUBSYS_COUNT; i++)
        root_cgroup.subsys[i] = NULL;

    /* 初始化各控制器（使用默认无限值） */
    cpu_cgroup_init(&root_cgroup);
    mem_cgroup_init(&root_cgroup);
    pids_cgroup_init(&root_cgroup);

    /* 初始化 init_css_set */
    init_css_set.cgrp = &root_cgroup;
    init_css_set.refcount = 1;

    /* 更新 cgroup namespace 的根引用 */
    init_cgroup_ns.root_cgrp = &root_cgroup;

    boot_printk("[cgroup] cgroup v2 initialized (root=\"/\")\n");
}

/*
 * ============================================================
 * cgroup_create — 创建子 cgroup
 *
 * @parent: 父 cgroup
 * @name:   子 cgroup 名称（如 "container1"）
 *
 * 子 cgroup 继承父 cgroup 的控制器参数（初始无限制）。
 *
 * 返回：新 cgroup 指针，NULL 表示失败。
 *
 * 参考：kernel/cgroup/cgroup.c cgroup_mkdir()
 * ============================================================
 */
struct cgroup *cgroup_create(struct cgroup *parent, const char *name)
{
    struct cgroup *cgrp;
    int i;

    if (!parent || !name) {
        boot_printk("[cgroup] ERROR: invalid parent or name\n");
        return NULL;
    }

    if (parent->nr_children >= CGROUP_MAX_CHILDREN) {
        boot_printk("[cgroup] ERROR: max children reached\n");
        return NULL;
    }

    if (cgroup_pool_idx >= MAX_CGROUPS) {
        boot_printk("[cgroup] ERROR: cgroup pool exhausted\n");
        return NULL;
    }

    cgrp = &cgroup_pool[cgroup_pool_idx++];
    cgrp->parent = parent;
    cgrp->nr_children = 0;
    cgrp->id = next_cgroup_id++;
    cgrp->nr_procs = 0;

    /* 设置名称 */
    for (i = 0; i < CGROUP_NAME_LEN - 1 && name[i]; i++)
        cgrp->name[i] = name[i];
    cgrp->name[i] = '\0';

    /* 清零子 cgroup 指针和控制器状态 */
    for (i = 0; i < CGROUP_MAX_CHILDREN; i++)
        cgrp->children[i] = NULL;
    for (i = 0; i < CGROUP_SUBSYS_COUNT; i++)
        cgrp->subsys[i] = NULL;

    /* 初始化各控制器（继承父默认值） */
    cpu_cgroup_init(cgrp);
    mem_cgroup_init(cgrp);
    pids_cgroup_init(cgrp);

    /* 注册到父 cgroup 的 children 数组 */
    parent->children[parent->nr_children++] = cgrp;

    boot_printk("[cgroup] Created cgroup: ");
    boot_printk(name);
    boot_printk(" (id=");
    boot_printk_hex(cgrp->id);
    boot_printk(")\n");

    return cgrp;
}

/*
 * ============================================================
 * cgroup_attach_task — 将进程附加到 cgroup
 *
 * @cgrp: 目标 cgroup
 * @task: 进程（task_struct）
 *
 * 更新进程的 css_set 指向新 cgroup，
 * 更新 cgroup 的进程计数。
 *
 * 返回 0 成功，-1 失败。
 *
 * 参考：kernel/cgroup/cgroup.c cgroup_attach_task()
 * ============================================================
 */
int cgroup_attach_task(struct cgroup *cgrp, struct task_struct *task)
{
    struct css_set *new_set;

    if (!cgrp || !task)
        return -1;

    /* 检查 PIDs 控制器限制 */
    if (!pids_cgroup_can_fork(cgrp)) {
        boot_printk("[cgroup] DENIED: pids.max reached for ");
        boot_printk(cgrp->name);
        boot_printk("\n");
        return -1;
    }

    /* 分配新 css_set */
    if (css_set_pool_idx >= MAX_CSS_SETS) {
        boot_printk("[cgroup] ERROR: css_set pool exhausted\n");
        return -1;
    }

    new_set = &css_set_pool[css_set_pool_idx++];
    new_set->cgrp = cgrp;
    new_set->refcount = 1;

    /* 从旧 cgroup 分离 */
    if (task->cgroups && task->cgroups->cgrp) {
        task->cgroups->cgrp->nr_procs--;
        pids_cgroup_uncharge(task->cgroups->cgrp);
    }

    /* 附加到新 cgroup */
    task->cgroups = new_set;
    cgrp->nr_procs++;
    pids_cgroup_charge(cgrp);

    return 0;
}

/*
 * ============================================================
 * cgroup_detach_task — 将进程从 cgroup 分离
 *
 * @cgrp: cgroup
 * @task: 进程
 *
 * 返回 0 成功，-1 失败。
 * ============================================================
 */
int cgroup_detach_task(struct cgroup *cgrp, struct task_struct *task)
{
    if (!cgrp || !task)
        return -1;

    if (task->cgroups && task->cgroups->cgrp == cgrp) {
        cgrp->nr_procs--;
        pids_cgroup_uncharge(cgrp);
        /* 回到根 cgroup */
        task->cgroups = &init_css_set;
        root_cgroup.nr_procs++;
    }

    return 0;
}

/*
 * cgroup_get — 获取 cgroup 引用
 */
struct cgroup *cgroup_get(struct cgroup *cgrp)
{
    return cgrp;
}
