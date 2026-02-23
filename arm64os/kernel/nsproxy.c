/* SPDX-License-Identifier: GPL-2.0 */
/*
 * arm64os/kernel/nsproxy.c
 *
 * Namespace 代理（核心）
 *
 * 参考：kernel/nsproxy.c
 *
 * Phase 10 实现：
 *   - init_nsproxy：初始 namespace 集合（所有进程的默认 namespace）
 *   - nsproxy_init()：初始化全局 nsproxy 池
 *   - create_nsproxy()：创建新的 nsproxy（根据 clone flags 创建新 namespace）
 *   - get_nsproxy() / put_nsproxy()：引用计数管理
 *
 * 每个进程通过 task_struct->nsproxy 持有一组 namespace 指针。
 * unshare() 或 clone() 时根据 flags 创建新的 namespace。
 */

#include <linux/types.h>
#include <linux/nsproxy.h>

/* 外部函数 */
void boot_printk(const char *s);
void boot_printk_hex(unsigned long val);

/*
 * ============================================================
 * 静态池
 * ============================================================
 */
static struct nsproxy nsproxy_pool[MAX_NSPROXIES];
static int nsproxy_pool_idx = 0;

/*
 * ============================================================
 * 初始 namespace 实例
 *
 * IPC、Network、Cgroup namespace 在此定义。
 * UTS、PID、User、Mount namespace 在各自的源文件中定义。
 * ============================================================
 */
struct ipc_namespace init_ipc_ns = {
    .count = 1,
    .id    = 0,
};

struct net init_net = {
    .count   = 1,
    .ifindex = 0,
};

struct cgroup_namespace init_cgroup_ns = {
    .count     = 1,
    .root_cgrp = NULL,  /* cgroup_init() 后更新 */
};

struct nsproxy init_nsproxy = {
    .count              = 1,
    .uts_ns             = &init_uts_ns,
    .ipc_ns             = &init_ipc_ns,
    .mnt_ns             = &init_mnt_ns,
    .pid_ns_for_children = &init_pid_ns,
    .net_ns             = &init_net,
    .cgroup_ns          = &init_cgroup_ns,
};

/*
 * ============================================================
 * nsproxy_init — 初始化 namespace 子系统
 *
 * 调用各 namespace 类型的初始化函数。
 * ============================================================
 */
void nsproxy_init(void)
{
    boot_printk("[nsproxy] Initializing namespace subsystem...\n");

    /* 初始化各 namespace 子系统 */
    uts_ns_init();
    pid_ns_init();
    user_ns_init();
    mnt_ns_init();

    /* IPC / Network / Cgroup namespace 已通过编译期初始化 */
    /* init_cgroup_ns.root_cgrp 将在 cgroup_init() 中设置 */

    boot_printk("[nsproxy] namespace init: PASS\n");
}

/*
 * ============================================================
 * create_nsproxy — 创建新的 nsproxy
 *
 * 根据 clone flags 选择性创建新的 namespace。
 * 未指定 flag 的 namespace 共享父进程的实例。
 *
 * @orig:  原始 nsproxy（通常是 current->nsproxy）
 * @flags: CLONE_NEW* 标志位的组合
 *
 * 返回：新的 nsproxy，NULL 表示池耗尽。
 *
 * 参考：kernel/nsproxy.c create_new_namespaces()
 * ============================================================
 */
struct nsproxy *create_nsproxy(struct nsproxy *orig, unsigned long flags)
{
    struct nsproxy *new;

    if (nsproxy_pool_idx >= MAX_NSPROXIES) {
        boot_printk("[nsproxy] ERROR: nsproxy pool exhausted\n");
        return NULL;
    }

    new = &nsproxy_pool[nsproxy_pool_idx++];
    new->count = 1;

    /* 默认继承父进程的 namespace */
    new->uts_ns             = orig->uts_ns;
    new->ipc_ns             = orig->ipc_ns;
    new->mnt_ns             = orig->mnt_ns;
    new->pid_ns_for_children = orig->pid_ns_for_children;
    new->net_ns             = orig->net_ns;
    new->cgroup_ns          = orig->cgroup_ns;

    /* 根据 flags 创建新的 namespace */
    if (flags & CLONE_NEWUTS) {
        new->uts_ns = create_uts_namespace(orig->uts_ns);
        if (!new->uts_ns) {
            boot_printk("[nsproxy] ERROR: create UTS ns failed\n");
            return NULL;
        }
    } else {
        orig->uts_ns->count++;
    }

    if (flags & CLONE_NEWPID) {
        new->pid_ns_for_children =
            create_pid_namespace(orig->pid_ns_for_children);
        if (!new->pid_ns_for_children) {
            boot_printk("[nsproxy] ERROR: create PID ns failed\n");
            return NULL;
        }
    } else {
        orig->pid_ns_for_children->count++;
    }

    if (flags & CLONE_NEWNS) {
        new->mnt_ns = create_mnt_namespace(orig->mnt_ns);
        if (!new->mnt_ns) {
            boot_printk("[nsproxy] ERROR: create MNT ns failed\n");
            return NULL;
        }
    } else {
        orig->mnt_ns->count++;
    }

    if (flags & CLONE_NEWIPC) {
        /* 简化：IPC namespace 只需分配新 ID */
        static int next_ipc_id = 1;
        static struct ipc_namespace ipc_pool[MAX_IPC_NS];
        static int ipc_idx = 0;
        if (ipc_idx < MAX_IPC_NS) {
            new->ipc_ns = &ipc_pool[ipc_idx++];
            new->ipc_ns->count = 1;
            new->ipc_ns->id = next_ipc_id++;
        }
    } else {
        orig->ipc_ns->count++;
    }

    if (!(flags & CLONE_NEWNET))
        orig->net_ns->count++;

    if (!(flags & CLONE_NEWCGROUP))
        orig->cgroup_ns->count++;

    return new;
}

/*
 * get_nsproxy / put_nsproxy — 引用计数管理
 */
struct nsproxy *get_nsproxy(struct nsproxy *ns)
{
    if (ns)
        ns->count++;
    return ns;
}

void put_nsproxy(struct nsproxy *ns)
{
    if (ns && --ns->count == 0) {
        /* 释放各 namespace 引用 */
        put_uts_ns(ns->uts_ns);
        put_pid_ns(ns->pid_ns_for_children);
        put_mnt_ns(ns->mnt_ns);
        /* IPC / net / cgroup 简化处理 */
        if (ns->ipc_ns)
            ns->ipc_ns->count--;
        if (ns->net_ns)
            ns->net_ns->count--;
        if (ns->cgroup_ns)
            ns->cgroup_ns->count--;
    }
}
