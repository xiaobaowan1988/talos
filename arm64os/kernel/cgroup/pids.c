/* SPDX-License-Identifier: GPL-2.0 */
/*
 * arm64os/kernel/cgroup/pids.c
 *
 * PIDs 控制器 — 进程数量限制
 *
 * 参考：kernel/cgroup/pids.c
 *
 * Phase 10 实现：
 *   - pids_cgroup_init()：初始化 PIDs 控制器参数
 *   - pids_cgroup_set_max()：设置 pids.max（最大进程数）
 *   - pids_cgroup_can_fork()：检查是否允许创建新进程
 *   - pids_cgroup_charge()：进程加入 cgroup 时增加计数
 *   - pids_cgroup_uncharge()：进程离开 cgroup 时减少计数
 *
 * cgroup v2 PIDs 控制器接口：
 *   pids.max     : 最大进程数，"max" = 无限制
 *   pids.current : 当前进程数（只读）
 *
 * 工作流程：
 *   1. fork/clone 时调用 pids_cgroup_can_fork()
 *   2. 若 pids_current >= pids_max，拒绝创建进程（返回 EAGAIN）
 *   3. 成功 fork 后调用 pids_cgroup_charge() 增加计数
 *   4. 进程退出时调用 pids_cgroup_uncharge() 减少计数
 */

#include <linux/types.h>
#include <linux/cgroup.h>

/* 外部函数 */
void boot_printk(const char *s);
void boot_printk_hex(unsigned long val);

/*
 * ============================================================
 * pids_cgroup_init — 初始化 cgroup 的 PIDs 控制器
 *
 * @cgrp: 目标 cgroup
 *
 * 默认值：无限制（pids_max = -1）
 * ============================================================
 */
void pids_cgroup_init(struct cgroup *cgrp)
{
    if (!cgrp)
        return;

    cgrp->pids_max = -1;       /* -1 = 无限制（"max"）*/
    cgrp->pids_current = 0;
}

/*
 * ============================================================
 * pids_cgroup_set_max — 设置最大进程数
 *
 * @cgrp: 目标 cgroup
 * @max:  最大进程数，-1 = 无限制
 *
 * 参考：kernel/cgroup/pids.c pids_max_write()
 * ============================================================
 */
void pids_cgroup_set_max(struct cgroup *cgrp, int max)
{
    if (!cgrp)
        return;

    cgrp->pids_max = max;

    boot_printk("[pids_cg] Set pids.max=");
    if (max < 0) {
        boot_printk("max");
    } else {
        boot_printk_hex((unsigned long)max);
    }
    boot_printk("\n");
}

/*
 * ============================================================
 * pids_cgroup_can_fork — 检查是否允许创建新进程
 *
 * @cgrp: 目标 cgroup
 *
 * 返回 1 表示允许，0 表示超限（拒绝 fork）。
 *
 * 参考：kernel/cgroup/pids.c pids_can_fork()
 * ============================================================
 */
int pids_cgroup_can_fork(struct cgroup *cgrp)
{
    if (!cgrp)
        return 1;

    /* 无限制 */
    if (cgrp->pids_max < 0)
        return 1;

    /* 检查是否超限 */
    if (cgrp->pids_current >= cgrp->pids_max) {
        boot_printk("[pids_cg] DENIED: pids.current=");
        boot_printk_hex((unsigned long)cgrp->pids_current);
        boot_printk(" >= pids.max=");
        boot_printk_hex((unsigned long)cgrp->pids_max);
        boot_printk("\n");
        return 0;   /* 拒绝 */
    }

    return 1;       /* 允许 */
}

/*
 * ============================================================
 * pids_cgroup_charge — 进程加入 cgroup，增加计数
 *
 * @cgrp: 目标 cgroup
 *
 * 参考：kernel/cgroup/pids.c pids_charge()
 * ============================================================
 */
void pids_cgroup_charge(struct cgroup *cgrp)
{
    if (!cgrp)
        return;

    cgrp->pids_current++;
}

/*
 * ============================================================
 * pids_cgroup_uncharge — 进程离开 cgroup，减少计数
 *
 * @cgrp: 目标 cgroup
 *
 * 参考：kernel/cgroup/pids.c pids_uncharge()
 * ============================================================
 */
void pids_cgroup_uncharge(struct cgroup *cgrp)
{
    if (!cgrp)
        return;

    if (cgrp->pids_current > 0)
        cgrp->pids_current--;
}
