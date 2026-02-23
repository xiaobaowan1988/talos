/* SPDX-License-Identifier: GPL-2.0 */
/*
 * arm64os/kernel/cgroup/cpu.c
 *
 * CPU 控制器 — CPU 时间限制与权重
 *
 * 参考：kernel/sched/core.c (cpu controller)
 *       kernel/cgroup/cgroup-v2.c
 *
 * Phase 10 实现：
 *   - cpu_cgroup_init()：初始化 CPU 控制器参数
 *   - cpu_cgroup_set_max()：设置 cpu.max（配额/周期）
 *   - cpu_cgroup_set_weight()：设置 cpu.weight（调度权重）
 *
 * cgroup v2 CPU 控制器接口：
 *   cpu.max     : "$MAX $PERIOD"（如 "100000 100000" = 100%）
 *                 $MAX 微秒 / $PERIOD 微秒 = CPU 时间比例
 *                 "max 100000" = 无限制
 *   cpu.weight  : 1-10000（默认 100）
 *                 CFS 调度权重（替代 nice 值）
 *   cpu.stat    : 只读，CPU 使用统计
 */

#include <linux/types.h>
#include <linux/cgroup.h>

/* 外部函数 */
void boot_printk(const char *s);
void boot_printk_hex(unsigned long val);

/*
 * ============================================================
 * cpu_cgroup_init — 初始化 cgroup 的 CPU 控制器
 *
 * @cgrp: 目标 cgroup
 *
 * 默认值：无限制（cpu_max_quota=0 表示 "max"）
 * ============================================================
 */
void cpu_cgroup_init(struct cgroup *cgrp)
{
    if (!cgrp)
        return;

    cgrp->cpu_max_quota = 0;         /* 0 = 无限制（"max"）*/
    cgrp->cpu_max_period = 100000;   /* 100ms 默认周期 */
    cgrp->cpu_weight = 100;          /* 默认权重 */
}

/*
 * ============================================================
 * cpu_cgroup_set_max — 设置 CPU 配额限制
 *
 * @cgrp:      目标 cgroup
 * @quota_us:  每周期允许使用的 CPU 时间（微秒），0 = 无限制
 * @period_us: 周期长度（微秒），0 = 使用默认值 100000
 *
 * 例：quota_us=50000, period_us=100000 → 50% CPU
 *     quota_us=200000, period_us=100000 → 200%（可用 2 个 CPU）
 *
 * 参考：kernel/sched/core.c cpu_max_write()
 * ============================================================
 */
void cpu_cgroup_set_max(struct cgroup *cgrp,
                         unsigned long quota_us, unsigned long period_us)
{
    if (!cgrp)
        return;

    cgrp->cpu_max_quota = quota_us;
    if (period_us > 0)
        cgrp->cpu_max_period = period_us;

    boot_printk("[cpu_cg] Set cpu.max: quota=");
    if (quota_us == 0) {
        boot_printk("max");
    } else {
        boot_printk_hex(quota_us);
    }
    boot_printk(" period=");
    boot_printk_hex(cgrp->cpu_max_period);
    boot_printk("us\n");
}

/*
 * ============================================================
 * cpu_cgroup_set_weight — 设置 CPU 调度权重
 *
 * @cgrp:   目标 cgroup
 * @weight: 权重值（1-10000，默认 100）
 *
 * 权重决定 CFS 调度器在同一层级的 cgroup 间分配 CPU 时间的比例。
 * 例：cgroup A weight=100, cgroup B weight=200
 *     → A 获得约 1/3 CPU 时间，B 获得约 2/3
 *
 * 参考：kernel/sched/core.c cpu_weight_write_u64()
 * ============================================================
 */
void cpu_cgroup_set_weight(struct cgroup *cgrp, unsigned long weight)
{
    if (!cgrp)
        return;

    /* 限制范围 */
    if (weight < 1)
        weight = 1;
    if (weight > 10000)
        weight = 10000;

    cgrp->cpu_weight = weight;

    boot_printk("[cpu_cg] Set cpu.weight=");
    boot_printk_hex(weight);
    boot_printk("\n");
}
