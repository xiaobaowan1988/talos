/* SPDX-License-Identifier: GPL-2.0 */
/*
 * arm64os/kernel/cgroup/memory.c
 *
 * Memory 控制器 — 内存使用限制与记账
 *
 * 参考：mm/memcontrol.c
 *       include/linux/memcontrol.h
 *
 * Phase 10 实现：
 *   - mem_cgroup_init()：初始化内存控制器参数
 *   - mem_cgroup_set_max()：设置 memory.max（内存上限）
 *   - mem_cgroup_charge()：记账（分配内存时增加计数）
 *   - mem_cgroup_uncharge()：释放记账
 *   - mem_cgroup_oom_check()：检查是否超限（触发 OOM）
 *
 * cgroup v2 Memory 控制器接口：
 *   memory.max     : 内存上限（字节），"max" = 无限制
 *   memory.current : 当前使用量（只读）
 *   memory.oom.group : OOM 时整体 kill（0/1）
 *
 * 工作流程：
 *   1. 进程分配页面时调用 mem_cgroup_charge()
 *   2. charge 检查 memory_current + nr_bytes 是否超过 memory_max
 *   3. 若超限：尝试回收内存 → 仍超限则触发 OOM kill
 *   4. 进程释放页面时调用 mem_cgroup_uncharge() 减少计数
 */

#include <linux/types.h>
#include <linux/cgroup.h>

/* 外部函数 */
void boot_printk(const char *s);
void boot_printk_hex(unsigned long val);

/*
 * ============================================================
 * mem_cgroup_init — 初始化 cgroup 的内存控制器
 *
 * @cgrp: 目标 cgroup
 *
 * 默认值：无限制
 * ============================================================
 */
void mem_cgroup_init(struct cgroup *cgrp)
{
    if (!cgrp)
        return;

    cgrp->memory_max = CGROUP_MAX_UNLIMITED;    /* 无限制 */
    cgrp->memory_current = 0;
}

/*
 * ============================================================
 * mem_cgroup_set_max — 设置内存上限
 *
 * @cgrp:      目标 cgroup
 * @max_bytes: 内存上限（字节），CGROUP_MAX_UNLIMITED = 无限制
 *
 * 参考：mm/memcontrol.c memory_max_write()
 * ============================================================
 */
void mem_cgroup_set_max(struct cgroup *cgrp, unsigned long max_bytes)
{
    if (!cgrp)
        return;

    cgrp->memory_max = max_bytes;

    boot_printk("[mem_cg] Set memory.max=");
    if (max_bytes == CGROUP_MAX_UNLIMITED) {
        boot_printk("max");
    } else {
        boot_printk_hex(max_bytes);
    }
    boot_printk(" bytes\n");
}

/*
 * ============================================================
 * mem_cgroup_charge — 记账：尝试分配内存
 *
 * @cgrp:     目标 cgroup
 * @nr_bytes: 本次分配的字节数
 *
 * 返回 0 表示允许分配，-1 表示超限（OOM）。
 *
 * 参考：mm/memcontrol.c mem_cgroup_charge()
 * ============================================================
 */
int mem_cgroup_charge(struct cgroup *cgrp, unsigned long nr_bytes)
{
    if (!cgrp)
        return 0;

    /* 无限制则直接通过 */
    if (cgrp->memory_max == CGROUP_MAX_UNLIMITED) {
        cgrp->memory_current += nr_bytes;
        return 0;
    }

    /* 检查是否超限 */
    if (cgrp->memory_current + nr_bytes > cgrp->memory_max) {
        boot_printk("[mem_cg] OOM: charge ");
        boot_printk_hex(nr_bytes);
        boot_printk(" bytes would exceed memory.max=");
        boot_printk_hex(cgrp->memory_max);
        boot_printk(" (current=");
        boot_printk_hex(cgrp->memory_current);
        boot_printk(")\n");
        return -1;  /* 超限 */
    }

    /* 增加计数 */
    cgrp->memory_current += nr_bytes;
    return 0;
}

/*
 * ============================================================
 * mem_cgroup_uncharge — 释放记账
 *
 * @cgrp:     目标 cgroup
 * @nr_bytes: 释放的字节数
 *
 * 参考：mm/memcontrol.c mem_cgroup_uncharge()
 * ============================================================
 */
void mem_cgroup_uncharge(struct cgroup *cgrp, unsigned long nr_bytes)
{
    if (!cgrp)
        return;

    if (cgrp->memory_current >= nr_bytes)
        cgrp->memory_current -= nr_bytes;
    else
        cgrp->memory_current = 0;
}

/*
 * ============================================================
 * mem_cgroup_oom_check — 检查 cgroup 是否超限
 *
 * @cgrp: 目标 cgroup
 *
 * 返回 1 表示超限（应触发 OOM kill），0 表示正常。
 *
 * 参考：mm/memcontrol.c mem_cgroup_oom()
 * ============================================================
 */
int mem_cgroup_oom_check(struct cgroup *cgrp)
{
    if (!cgrp)
        return 0;

    if (cgrp->memory_max == CGROUP_MAX_UNLIMITED)
        return 0;

    return (cgrp->memory_current > cgrp->memory_max) ? 1 : 0;
}
