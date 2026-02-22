# Phase 10：Linux Namespaces + cgroup v2

## 参考内核文件

```
kernel/nsproxy.c            # namespace代理（每进程ns集合）
kernel/pid_namespace.c      # PID namespace
kernel/user_namespace.c     # User namespace（UID/GID映射）
fs/mount.c                  # Mount namespace
net/core/net_namespace.c    # Network namespace
kernel/utsname.c            # UTS namespace（主机名）
kernel/ipc/namespace.c      # IPC namespace
kernel/cgroup/cgroup.c      # cgroup v2核心
kernel/cgroup/cgroup-v2.c   # cgroup v2 unified hierarchy
kernel/cgroup/cpu.c         # CPU controller
kernel/cgroup/memory.c      # Memory controller（内存限制）
kernel/cgroup/pids.c        # PID controller（进程数限制）
include/linux/nsproxy.h     # nsproxy 结构定义
include/linux/cgroup.h      # cgroup 数据结构
```

---

## 10.1 Namespace概览

Linux 有 7 种 namespace，每种隔离系统资源的不同方面：

```
Namespace   | 隔离内容              | clone flag        | 内核文件
---------------------------------------------------------------------------
Mount (mnt) | 文件系统挂载点        | CLONE_NEWNS       | fs/mount.c
UTS         | 主机名/域名           | CLONE_NEWUTS      | kernel/utsname.c
IPC         | System V IPC/POSIX MQ | CLONE_NEWIPC      | kernel/ipc/namespace.c
PID         | 进程ID空间            | CLONE_NEWPID      | kernel/pid_namespace.c
Network     | 网络设备/IP/端口      | CLONE_NEWNET      | net/core/net_namespace.c
User        | UID/GID映射           | CLONE_NEWUSER     | kernel/user_namespace.c
Cgroup      | cgroup根              | CLONE_NEWCGROUP   | kernel/cgroup/namespace.c
```

## 10.2 nsproxy（namespace代理）

```c
/* 参考 include/linux/nsproxy.h */
/* 每个进程通过 task_struct->nsproxy 持有一组 namespace */
struct nsproxy {
    atomic_t        count;          /* 引用计数（共享ns的进程数）*/
    struct uts_namespace    *uts_ns;
    struct ipc_namespace    *ipc_ns;
    struct mnt_namespace    *mnt_ns;
    struct pid_namespace    *pid_ns_for_children;
    struct net              *net_ns;
    struct time_namespace   *time_ns;
    struct cgroup_namespace *cgroup_ns;
};

/* 初始 namespace（所有进程的默认namespace）*/
struct nsproxy init_nsproxy = {
    .count      = ATOMIC_INIT(1),
    .uts_ns     = &init_uts_ns,
    .ipc_ns     = &init_ipc_ns,
    .mnt_ns     = NULL,  /* 由 vfs_kern_mount 初始化 */
    .pid_ns_for_children = &init_pid_ns,
    .net_ns     = &init_net,
    .cgroup_ns  = &init_cgroup_ns,
};
```

## 10.3 PID Namespace

```c
/* 参考 kernel/pid_namespace.c */
/*
 * PID namespace 为容器提供独立的进程ID空间：
 * - 容器内的 init 进程 PID=1
 * - 容器外（宿主机）可以看到容器进程的真实PID
 * - 每层namespace有自己的PID视图
 */

struct pid_namespace {
    struct kref         kref;
    struct pidmap       pidmap[PIDMAP_ENTRIES]; /* PID位图 */
    int                 last_pid;
    unsigned int        nr_hashed;
    struct task_struct  *child_reaper; /* 容器内的 init 进程 */
    struct kmem_cache   *pid_cachep;
    unsigned int        level;         /* 嵌套深度（0=根namespace）*/
    struct pid_namespace *parent;
};

/* unshare(CLONE_NEWPID) 创建新 PID namespace */
int create_pid_namespace(struct pid_namespace *parent) {
    struct pid_namespace *ns;
    ns = kmalloc(sizeof(*ns));
    ns->level = parent->level + 1;
    ns->parent = get_pid_ns(parent);
    /* 新namespace从PID 1开始分配 */
    ns->last_pid = 0;
    return 0;
}

/* 进程在不同namespace中有不同PID */
/* task_pid_vnr(task) 返回当前namespace视角的PID */
pid_t task_pid_vnr(struct task_struct *tsk) {
    return pid_nr_ns(task_pid(tsk), current->nsproxy->pid_ns_for_children);
}
```

## 10.4 Mount Namespace

```c
/* 参考 fs/mount.c */
/*
 * Mount namespace 隔离挂载点视图：
 * clone(CLONE_NEWNS) 或 unshare(CLONE_NEWNS) 创建新的挂载命名空间
 * 新namespace是父namespace挂载树的副本（写时复制）
 */

struct mnt_namespace {
    struct ns_common    ns;
    struct mount        *root;      /* 根挂载点 */
    struct list_head    list;       /* 所有挂载点链表 */
    spinlock_t          ns_lock;
    u64                 seq;        /* 修改序号 */
    wait_queue_head_t   poll;
    u64                 event;
    unsigned int        mounts;     /* 挂载点数量 */
};

/* 容器启动时：pivot_root 切换根文件系统 */
int pivot_root(const char *new_root, const char *put_old) {
    /* 将 new_root 设置为进程的根目录 */
    /* 将原根目录挂到 put_old 下（之后可umount）*/
    /* 这是容器化的关键系统调用 */
}
```

## 10.5 Network Namespace

```c
/* 参考 net/core/net_namespace.c */
/*
 * Network namespace 给容器独立的网络栈：
 * - 独立的网络设备（lo, eth0...）
 * - 独立的路由表、iptables规则
 * - 独立的端口空间（容器可以监听任意端口）
 */

struct net {
    /* 网络设备列表 */
    struct list_head    dev_base_head;
    struct hlist_head   *dev_name_head; /* 按名字索引 */
    struct hlist_head   *dev_index_head;/* 按ifindex索引 */

    /* 路由表 */
    struct fib_table    *fib_local;
    struct fib_table    *fib_main;

    /* 协议统计 */
    struct netns_mib    mib;

    /* iptables */
    struct netns_xt     xt;

    /* lo 回环设备 */
    struct net_device   *loopback_dev;

    unsigned int        ifindex;  /* 下一个接口索引 */
};

/* veth pair：连接两个network namespace */
/* ip link add veth0 type veth peer name veth1 */
/* ip link set veth1 netns <container_ns> */
```

## 10.6 cgroup v2 统一层级

cgroup v2 使用单一层级（unified hierarchy），所有控制器挂在同一棵树上：

```
cgroup v2 文件系统（/sys/fs/cgroup/）结构：

/sys/fs/cgroup/
├── cgroup.controllers        # 所有可用控制器
├── cgroup.subtree_control    # 当前启用的控制器
├── cgroup.procs              # 根cgroup的进程列表
│
└── container1/               # 容器 cgroup
    ├── cgroup.controllers
    ├── cgroup.procs          # 将容器PID写入此文件
    ├── cpu.max               # CPU限制：如 "100000 100000"（100%）
    ├── cpu.weight            # CPU权重（替代CFS nice值）
    ├── memory.max            # 内存上限：如 "524288000"（500MB）
    ├── memory.current        # 当前内存使用量（只读）
    ├── memory.oom.group      # OOM时整体kill
    ├── pids.max              # 最大进程数
    └── pids.current          # 当前进程数（只读）
```

## 10.7 cgroup核心数据结构

```c
/* 参考 include/linux/cgroup.h */

/* cgroup 节点 */
struct cgroup {
    struct cgroup_subsys_state __rcu *subsys[CGROUP_SUBSYS_COUNT];
    struct cgroup_root  *root;      /* 所属层级根 */
    struct cgroup       *parent;
    struct list_head     children;
    struct list_head     cset_links; /* 关联的css_set */
    /* cgroupfs 对应的 kernfs 节点 */
    struct kernfs_node  *kn;
    /* cgroup 名称（目录名）*/
    char                *name;
    int                  id;
};

/* 每个cgroup子系统的状态（嵌入进程task_struct）*/
struct cgroup_subsys_state {
    struct cgroup       *cgroup;
    struct cgroup_subsys *ss;
    struct percpu_ref    refcnt;
    struct cgroup_subsys_state *parent;
};

/* 每个进程持有一个 css_set（指向其所在各子系统的状态）*/
struct css_set {
    struct cgroup_subsys_state *subsys[CGROUP_SUBSYS_COUNT];
    refcount_t refcount;
    struct list_head tasks;  /* 该css_set下的所有进程 */
};

/* task_struct 中 */
struct task_struct {
    /* ... */
    struct css_set __rcu *cgroups;  /* 所属的css_set */
    struct list_head cg_list;       /* css_set->tasks 链表节点 */
};
```

## 10.8 Memory Controller实现（参考 kernel/cgroup/memory.c）

```c
/* 参考 mm/memcontrol.c */

struct mem_cgroup {
    struct cgroup_subsys_state css;
    struct page_counter memory;   /* 内存计数器（含上限）*/
    struct page_counter swap;
    /* OOM 控制 */
    bool    oom_kill_disable;
    /* 统计 */
    struct mem_cgroup_stat_cpu __percpu *stat;
};

/* 分配页面时检查 memory controller 限制 */
int mem_cgroup_charge(struct page *page, struct mm_struct *mm, gfp_t gfp) {
    struct mem_cgroup *memcg = get_mem_cgroup_from_mm(mm);

    /* 尝试增加计数，若超限则触发回收或OOM */
    if (page_counter_try_charge(&memcg->memory, 1, &counter)) {
        /* 成功：page->mem_cgroup = memcg */
        commit_charge(page, memcg);
        return 0;
    }

    /* 超限：尝试回收内存 */
    if (try_to_free_mem_cgroup_pages(memcg, gfp))
        goto retry;

    /* 仍然超限：OOM kill */
    mem_cgroup_oom(memcg, gfp, get_order(PAGE_SIZE));
    return -ENOMEM;
}
```

## 10.9 容器创建完整流程

```c
/*
 * 模拟 containerd/runc 创建容器的 namespace + cgroup 步骤
 */
void create_container(const char *image_sqsh) {
    /* 1. 创建 cgroup */
    mkdir("/sys/fs/cgroup/container1", 0755);
    write_file("/sys/fs/cgroup/container1/memory.max", "512M");
    write_file("/sys/fs/cgroup/container1/pids.max", "100");

    /* 2. clone 创建新进程，同时创建所有namespace */
    int flags = CLONE_NEWNS | CLONE_NEWPID | CLONE_NEWNET |
                CLONE_NEWUTS | CLONE_NEWIPC | CLONE_NEWUSER;
    pid_t child = clone(container_init, stack_top, flags, NULL);

    /* 3. 将子进程加入 cgroup */
    char pid_str[16];
    sprintf(pid_str, "%d", child);
    write_file("/sys/fs/cgroup/container1/cgroup.procs", pid_str);

    /* 子进程（容器init）中：*/
    /* pivot_root 到 overlayfs 挂载点 */
    /* exec 容器入口程序 */
}
```

## 10.10 验证方法

```c
void test_namespaces(void) {
    /* 创建 PID namespace */
    unshare(CLONE_NEWPID);

    pid_t pid = fork();
    if (pid == 0) {
        /* 子进程在新PID namespace中，PID=1 */
        printk("Container init PID: %d\n", getpid());  /* 应打印 1 */
        exit(0);
    }

    /* 验证 cgroup 内存限制 */
    write_file("/sys/fs/cgroup/test/memory.max", "10485760");  /* 10MB */
    write_file("/sys/fs/cgroup/test/cgroup.procs", "self");

    /* 尝试分配 20MB，应触发OOM kill */
    char *buf = malloc(20 * 1024 * 1024);
    memset(buf, 0, 20 * 1024 * 1024);
    printk("If we get here, memory limit not enforced\n");
}
```

## 10.11 本阶段产出文件

```
arm64os/
└── kernel/
    ├── nsproxy.c          ← namespace代理（核心）
    ├── pid_namespace.c    ← PID namespace
    ├── user_namespace.c   ← User namespace（UID映射）
    ├── utsname.c          ← UTS namespace
    ├── cgroup/
    │   ├── cgroup.c       ← cgroup v2核心（核心）
    │   ├── cpu.c          ← CPU控制器
    │   ├── memory.c       ← 内存控制器（核心）
    │   └── pids.c         ← PID数量控制器
    └── fs/
        └── mount.c        ← Mount namespace（更新）
```
