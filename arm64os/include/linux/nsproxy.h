/* SPDX-License-Identifier: GPL-2.0 */
/*
 * arm64os/include/linux/nsproxy.h
 *
 * Namespace proxy + 各 namespace 类型定义
 *
 * 参考：include/linux/nsproxy.h
 *       include/linux/utsname.h
 *       include/linux/ipc_namespace.h
 *       include/linux/pid_namespace.h
 *       include/linux/user_namespace.h
 *       fs/mount.h
 *
 * Phase 10 实现 7 种 Linux namespace：
 *   - Mount (mnt)   : 文件系统挂载点隔离
 *   - UTS           : 主机名/域名隔离
 *   - IPC           : System V IPC 隔离
 *   - PID           : 进程 ID 空间隔离
 *   - Network (net) : 网络栈隔离（Phase 11 实现，此处占位）
 *   - User          : UID/GID 映射
 *   - Cgroup        : cgroup 根隔离
 *
 * 每个进程通过 task_struct->nsproxy 持有一组 namespace。
 *
 * 简化说明：
 *   - 使用静态池分配（无 slab/kmalloc）
 *   - 引用计数使用普通 int
 *   - 无 RCU
 */

#ifndef __LINUX_NSPROXY_H
#define __LINUX_NSPROXY_H

#include <linux/types.h>

/* ---- clone flags（namespace 创建标志）---- */
#define CLONE_NEWNS         0x00020000  /* Mount namespace */
#define CLONE_NEWUTS        0x04000000  /* UTS namespace */
#define CLONE_NEWIPC        0x08000000  /* IPC namespace */
#define CLONE_NEWPID        0x20000000  /* PID namespace */
#define CLONE_NEWNET        0x40000000  /* Network namespace */
#define CLONE_NEWUSER       0x10000000  /* User namespace */
#define CLONE_NEWCGROUP     0x02000000  /* Cgroup namespace */

/* ---- 静态池大小 ---- */
#define MAX_NSPROXIES       8
#define MAX_PID_NS          8
#define MAX_UTS_NS          8
#define MAX_IPC_NS          8
#define MAX_MNT_NS          8
#define MAX_USER_NS         8
#define MAX_CGROUP_NS       8

/*
 * ============================================================
 * UTS namespace — 主机名/域名隔离
 *
 * 参考：include/linux/utsname.h struct uts_namespace
 * ============================================================
 */
#define UTS_NODENAME_LEN    65
#define UTS_DOMAINNAME_LEN  65

struct uts_namespace {
    int                 count;          /* 引用计数 */
    char                nodename[UTS_NODENAME_LEN];     /* 主机名 */
    char                domainname[UTS_DOMAINNAME_LEN]; /* 域名 */
};

/*
 * ============================================================
 * IPC namespace — System V IPC 隔离（简化版）
 *
 * 参考：include/linux/ipc_namespace.h struct ipc_namespace
 * ============================================================
 */
struct ipc_namespace {
    int                 count;          /* 引用计数 */
    int                 id;             /* namespace ID（隔离标识）*/
};

/*
 * ============================================================
 * PID namespace — 进程 ID 空间隔离
 *
 * 参考：kernel/pid_namespace.c
 *
 * PID namespace 为容器提供独立的进程 ID 空间：
 *   - 容器内的 init 进程 PID=1
 *   - 容器外（宿主）可看到容器进程的真实 PID
 *   - 每层 namespace 有自己的 PID 视图
 * ============================================================
 */
#define PID_NS_MAX_PIDS     64          /* 每个 PID namespace 最大进程数 */

struct task_struct;  /* 前向声明 */

struct pid_namespace {
    int                 count;          /* 引用计数 */
    int                 last_pid;       /* 上次分配的 PID */
    unsigned int        level;          /* 嵌套深度（0=根 namespace）*/
    struct pid_namespace *parent;       /* 父 namespace */
    struct task_struct  *child_reaper;  /* 容器内的 init 进程 */
};

/*
 * ============================================================
 * User namespace — UID/GID 映射
 *
 * 参考：include/linux/user_namespace.h struct user_namespace
 *
 * 容器内的 UID 0 (root) 映射到宿主的非特权 UID。
 * ============================================================
 */
#define UID_MAP_MAX_ENTRIES 4

struct uid_gid_map_entry {
    unsigned int        inner_start;    /* 容器内起始 UID/GID */
    unsigned int        outer_start;    /* 宿主起始 UID/GID */
    unsigned int        count;          /* 映射范围 */
};

struct user_namespace {
    int                 refcount;       /* 引用计数 */
    struct user_namespace *parent;      /* 父 namespace */
    /* UID 映射表 */
    struct uid_gid_map_entry uid_map[UID_MAP_MAX_ENTRIES];
    int                 uid_map_nr;     /* 有效 UID 映射条目数 */
    /* GID 映射表 */
    struct uid_gid_map_entry gid_map[UID_MAP_MAX_ENTRIES];
    int                 gid_map_nr;     /* 有效 GID 映射条目数 */
};

/*
 * ============================================================
 * Mount namespace — 文件系统挂载点隔离
 *
 * 参考：fs/mount.h struct mnt_namespace
 *
 * 每个 mount namespace 维护独立的挂载表。
 * clone(CLONE_NEWNS) 创建新的 mount namespace，
 * 新 namespace 是父 namespace 挂载表的副本。
 * ============================================================
 */
#define MNT_NS_MAX_MOUNTS  8
#define MNT_NS_MAX_PATH    32

struct mnt_ns_entry {
    char                path[MNT_NS_MAX_PATH];  /* 挂载路径 */
    int                 pathlen;                /* 路径长度 */
    char                fstype[16];             /* 文件系统类型名 */
    int                 used;                   /* 是否已使用 */
};

struct mnt_namespace {
    int                 count;                  /* 引用计数 */
    struct mnt_ns_entry mounts[MNT_NS_MAX_MOUNTS]; /* 挂载表 */
    unsigned int        nr_mounts;              /* 挂载点数量 */
};

/*
 * ============================================================
 * Cgroup namespace — cgroup 根隔离（简化版）
 *
 * 参考：include/linux/cgroup.h struct cgroup_namespace
 * ============================================================
 */
struct cgroup;  /* 前向声明 */

struct cgroup_namespace {
    int                 count;          /* 引用计数 */
    struct cgroup      *root_cgrp;     /* 该 namespace 的 cgroup 根 */
};

/*
 * ============================================================
 * Network namespace — Phase 11 实现
 *
 * 参考：include/net/net_namespace.h struct net
 *
 * Phase 11 新增：
 *   - nf_hooks_ipv4[]：每个 netfilter Hook 点的钩子数组指针
 *     5 个 Hook 点对应 NF_INET_PRE_ROUTING .. NF_INET_POST_ROUTING
 * ============================================================
 */
struct nf_hook_entries;  /* 前向声明（定义在 include/linux/netfilter.h）*/

struct net {
    int                 count;          /* 引用计数 */
    int                 ifindex;        /* 下一个接口索引 */
    /* Phase 11: netfilter Hook 点（每个 Hook 点一个 entries 指针）*/
    struct nf_hook_entries *nf_hooks_ipv4[5];   /* NF_INET_NUMHOOKS = 5 */
};

/*
 * ============================================================
 * nsproxy — namespace 代理（每进程 namespace 集合）
 *
 * 参考：include/linux/nsproxy.h struct nsproxy
 *
 * 每个进程通过 task_struct->nsproxy 持有一组 namespace 指针。
 * 多个进程可共享同一个 nsproxy（引用计数管理）。
 * ============================================================
 */
struct nsproxy {
    int                         count;          /* 引用计数 */
    struct uts_namespace       *uts_ns;
    struct ipc_namespace       *ipc_ns;
    struct mnt_namespace       *mnt_ns;
    struct pid_namespace       *pid_ns_for_children;
    struct net                 *net_ns;
    struct cgroup_namespace    *cgroup_ns;
};

/*
 * ============================================================
 * 外部接口
 * ============================================================
 */

/* 初始 namespace 实例（所有进程的默认 namespace） */
extern struct nsproxy         init_nsproxy;
extern struct uts_namespace   init_uts_ns;
extern struct ipc_namespace   init_ipc_ns;
extern struct pid_namespace   init_pid_ns;
extern struct mnt_namespace   init_mnt_ns;
extern struct user_namespace  init_user_ns;
extern struct cgroup_namespace init_cgroup_ns;
extern struct net             init_net;

/* --- nsproxy.c --- */
void nsproxy_init(void);
struct nsproxy *create_nsproxy(struct nsproxy *orig, unsigned long flags);
struct nsproxy *get_nsproxy(struct nsproxy *ns);
void put_nsproxy(struct nsproxy *ns);

/* --- pid_namespace.c --- */
void pid_ns_init(void);
struct pid_namespace *create_pid_namespace(struct pid_namespace *parent);
int alloc_pid_nr(struct pid_namespace *ns);
int pid_nr_ns(int global_pid, struct pid_namespace *ns);
struct pid_namespace *get_pid_ns(struct pid_namespace *ns);
void put_pid_ns(struct pid_namespace *ns);

/* --- utsname.c --- */
void uts_ns_init(void);
struct uts_namespace *create_uts_namespace(struct uts_namespace *orig);
void uts_ns_set_hostname(struct uts_namespace *ns, const char *name);
struct uts_namespace *get_uts_ns(struct uts_namespace *ns);
void put_uts_ns(struct uts_namespace *ns);

/* --- user_namespace.c --- */
void user_ns_init(void);
struct user_namespace *create_user_namespace(struct user_namespace *parent);
int user_ns_map_uid(struct user_namespace *ns, unsigned int inner_uid);
void user_ns_set_uid_map(struct user_namespace *ns,
                          unsigned int inner, unsigned int outer,
                          unsigned int count);
struct user_namespace *get_user_ns(struct user_namespace *ns);
void put_user_ns(struct user_namespace *ns);

/* --- fs/mount.c (mount namespace) --- */
void mnt_ns_init(void);
struct mnt_namespace *create_mnt_namespace(struct mnt_namespace *orig);
int mnt_ns_add_mount(struct mnt_namespace *ns, const char *path,
                      const char *fstype);
int mnt_ns_has_mount(struct mnt_namespace *ns, const char *path);
struct mnt_namespace *get_mnt_ns(struct mnt_namespace *ns);
void put_mnt_ns(struct mnt_namespace *ns);

#endif /* __LINUX_NSPROXY_H */
