# Phase 10 Walkthrough: Linux Namespaces + cgroup v2

> **目标**：实现容器隔离（namespace）和资源限制（cgroup）。
> **最终效果**：PID/UTS/Mount/User namespace 隔离验证 + cgroup 内存/PIDs 限制。

---

## 10.1 容器 = namespace + cgroup + overlayfs

Docker 容器不是虚拟机，它用三种内核机制实现隔离：

```
  ┌──────────────────────────────────────────┐
  │              容器                         │
  │                                          │
  │  Namespace (隔离什么能看到)               │
  │    ├─ PID ns    → 进程只看到自己的 PID 树 │
  │    ├─ UTS ns    → 独立的主机名           │
  │    ├─ Mount ns  → 独立的挂载表           │
  │    ├─ Net ns    → 独立的网络栈           │
  │    ├─ User ns   → UID 映射 (容器内 root) │
  │    ├─ IPC ns    → 独立的 IPC            │
  │    └─ Cgroup ns → 独立的 cgroup 视图    │
  │                                          │
  │  cgroup v2 (限制能用多少)                │
  │    ├─ CPU       → CPU 时间配额          │
  │    ├─ Memory    → 内存上限              │
  │    └─ PIDs      → 最大进程数            │
  │                                          │
  │  overlayfs (文件系统隔离)  ← Phase 9     │
  │                                          │
  └──────────────────────────────────────────┘
```

---

## 10.2 nsproxy — Namespace 代理

每个进程有一个 `nsproxy` 结构，指向它所属的各种 namespace：

```c
struct nsproxy {
    struct uts_namespace *uts_ns;       /* 主机名 */
    struct ipc_namespace *ipc_ns;      /* IPC */
    struct mnt_namespace *mnt_ns;      /* 挂载表 */
    struct pid_namespace *pid_ns_for_children;  /* PID */
    struct net *net_ns;                /* 网络 */
    struct cgroup_namespace *cgroup_ns; /* cgroup */
};
```

`clone()` 系统调用的 CLONE_NEW* 标志控制哪些 namespace 创建新的：

```c
struct nsproxy *create_nsproxy(struct nsproxy *parent, unsigned long flags)
{
    struct nsproxy *new = alloc_nsproxy();

    /* 默认继承父进程的 namespace */
    *new = *parent;

    /* 根据标志创建新的 namespace */
    if (flags & CLONE_NEWUTS)
        new->uts_ns = create_uts_namespace(parent->uts_ns);
    if (flags & CLONE_NEWPID)
        new->pid_ns_for_children = create_pid_namespace(parent->pid_ns_for_children);
    if (flags & CLONE_NEWNS)
        new->mnt_ns = create_mnt_namespace(parent->mnt_ns);
    if (flags & CLONE_NEWUSER)
        new->user_ns = create_user_namespace(parent->user_ns);
    /* ... */

    return new;
}
```

---

## 10.3 PID Namespace — 进程 ID 隔离

```
  宿主机视角:                    容器视角:
  PID 1 (systemd)               PID 1 (容器 init) ← 实际是宿主 PID 42
  PID 2 (kthreadd)              PID 2 (nginx)     ← 实际是宿主 PID 43
  ...
  PID 42 (容器 init)
  PID 43 (nginx)
```

```c
struct pid_namespace {
    unsigned int level;          /* 嵌套深度（0=根） */
    struct pid_namespace *parent; /* 父 namespace */
    int last_pid;                /* 最后分配的 PID */
    int nr_procs;                /* 当前进程数 */
};

struct pid_namespace *create_pid_namespace(struct pid_namespace *parent)
{
    struct pid_namespace *ns = alloc_pid_ns();
    ns->parent = parent;
    ns->level = parent->level + 1;
    ns->last_pid = 0;  /* 新 namespace 从 PID 1 开始 */
    ns->nr_procs = 0;
    return ns;
}

int alloc_pid_nr(struct pid_namespace *ns)
{
    return ++(ns->last_pid);  /* 容器内第一个进程 = PID 1 */
}
```

---

## 10.4 UTS Namespace — 主机名隔离

```c
struct uts_namespace {
    char nodename[65];    /* 主机名 */
    char domainname[65];  /* 域名 */
    int ref_count;
};

struct uts_namespace *create_uts_namespace(struct uts_namespace *parent)
{
    struct uts_namespace *ns = alloc_uts_ns();
    /* 复制父 namespace 的主机名 */
    memcpy(ns->nodename, parent->nodename, 65);
    memcpy(ns->domainname, parent->domainname, 65);
    return ns;
}

void uts_ns_set_hostname(struct uts_namespace *ns, const char *name)
{
    int i;
    for (i = 0; name[i] && i < 64; i++)
        ns->nodename[i] = name[i];
    ns->nodename[i] = '\0';
}
```

**效果**：容器 A 的 `hostname` 是 "container-a"，容器 B 是 "container-b"，宿主是 "arm64os"。

---

## 10.5 Mount Namespace — 挂载表隔离

```c
struct mnt_namespace {
    struct mount_entry entries[8];  /* 挂载表副本 */
    int count;
    int ref_count;
};

struct mnt_namespace *create_mnt_namespace(struct mnt_namespace *parent)
{
    struct mnt_namespace *ns = alloc_mnt_ns();
    /* 复制父 namespace 的挂载表 */
    ns->count = parent->count;
    for (int i = 0; i < parent->count; i++)
        ns->entries[i] = parent->entries[i];
    return ns;
}
```

容器可以独立挂载/卸载文件系统，不影响宿主和其他容器。

---

## 10.6 User Namespace — UID 映射

```c
struct uid_gid_map {
    unsigned int inner_start;  /* 容器内起始 UID */
    unsigned int outer_start;  /* 宿主起始 UID */
    unsigned int count;        /* 映射范围大小 */
};

struct user_namespace {
    struct uid_gid_map uid_map;
    struct uid_gid_map gid_map;
    struct user_namespace *parent;
};
```

**经典映射**：容器内 UID 0 (root) → 宿主 UID 1000 (普通用户)

```c
/* 设置映射: 容器内 0-65535 → 宿主 1000-66535 */
user_ns_set_uid_map(ns, 0, 1000, 65536);

/* 翻译: 容器内 UID 0 → 宿主 UID ? */
unsigned int host_uid = user_ns_map_uid(ns, 0);
/* host_uid = 1000 */
```

---

## 10.7 cgroup v2 — 资源限制

### 统一层级结构

```
  /sys/fs/cgroup/          ← 根 cgroup
  ├── cgroup.procs         ← 写入 PID 加入此 cgroup
  ├── cpu.max              ← CPU 配额
  ├── memory.max           ← 内存上限
  ├── pids.max             ← 最大进程数
  └── container1/          ← 子 cgroup
      ├── cgroup.procs
      ├── cpu.max = "50000 100000"   ← 50% CPU
      ├── memory.max = "67108864"    ← 64MB
      └── pids.max = "100"           ← 最多 100 个进程
```

### 数据结构

```c
struct cgroup {
    char name[32];
    struct cgroup *parent;
    struct list_head children;
    struct list_head sibling;
    int nr_procs;                    /* 当前进程数 */

    /* 控制器状态 */
    unsigned long cpu_weight;        /* CPU 权重 */
    unsigned long cpu_max_quota;     /* CPU 配额 */
    unsigned long cpu_max_period;    /* CPU 周期 */
    unsigned long memory_max;        /* 内存上限 (字节) */
    unsigned long memory_current;    /* 当前内存使用 */
    unsigned long pids_max;          /* 最大进程数 */
    unsigned long pids_current;      /* 当前进程数 */
};
```

### 内存控制器

```c
int mem_cgroup_charge(struct cgroup *cgrp, unsigned long nr_bytes)
{
    /* 检查是否超限 */
    if (cgrp->memory_max > 0 &&
        cgrp->memory_current + nr_bytes > cgrp->memory_max) {
        return -ENOMEM;  /* 拒绝分配 */
    }

    cgrp->memory_current += nr_bytes;
    return 0;
}

void mem_cgroup_uncharge(struct cgroup *cgrp, unsigned long nr_bytes)
{
    if (cgrp->memory_current >= nr_bytes)
        cgrp->memory_current -= nr_bytes;
    else
        cgrp->memory_current = 0;
}
```

### PIDs 控制器

```c
int pids_cgroup_can_fork(struct cgroup *cgrp)
{
    if (cgrp->pids_max > 0 &&
        cgrp->pids_current >= cgrp->pids_max) {
        return -EAGAIN;  /* 拒绝 fork */
    }
    return 0;
}

void pids_cgroup_charge(struct cgroup *cgrp)
{
    cgrp->pids_current++;
}
```

### 将进程加入 cgroup

```c
int cgroup_attach_task(struct cgroup *cgrp, struct task_struct *task)
{
    /* 检查 PIDs 限制 */
    if (pids_cgroup_can_fork(cgrp) != 0)
        return -EAGAIN;

    /* 分配 css_set */
    struct css_set *cset = alloc_css_set();
    cset->cgrp = cgrp;
    task->cgroups = cset;

    /* 更新计数 */
    cgrp->nr_procs++;
    pids_cgroup_charge(cgrp);

    return 0;
}
```

---

## 10.8 容器创建完整流程

```
  1. 创建 cgroup 子目录
     cgroup_create(root, "container1")
     设置资源限制: memory.max=64MB, pids.max=100

  2. clone() 创建子进程，传入 CLONE_NEW* 标志
     clone(CLONE_NEWPID | CLONE_NEWUTS | CLONE_NEWNS | CLONE_NEWUSER)
     → 创建新 nsproxy
     → 每种 CLONE_NEW* 创建新 namespace

  3. 将子进程加入 cgroup
     cgroup_attach_task(container1_cgrp, child_task)

  4. 子进程进入新 namespace
     → PID namespace: 自己的 PID = 1（init 进程）
     → UTS namespace: 设置独立主机名
     → Mount namespace: 独立挂载表 (pivot_root 到 overlayfs)
     → User namespace: UID 0 映射到宿主非特权 UID
```

---

## 10.9 测试验证

```c
static void test_phase10(void)
{
    /* PID namespace 测试 */
    struct pid_namespace *child_pidns =
        create_pid_namespace(init_pid_ns);
    int pid = alloc_pid_nr(child_pidns);
    /* pid == 1 (新 namespace 从 1 开始) */

    /* UTS namespace 测试 */
    struct uts_namespace *child_uts =
        create_uts_namespace(init_uts_ns);
    uts_ns_set_hostname(child_uts, "container-1");
    /* 宿主 hostname 不受影响 */

    /* cgroup 内存限制测试 */
    struct cgroup *cg = cgroup_create(root_cgroup, "test");
    cg->memory_max = 4096;  /* 4KB 限制 */
    int ret = mem_cgroup_charge(cg, 8192);
    /* ret == -ENOMEM (超限拒绝) */

    /* cgroup PIDs 限制测试 */
    cg->pids_max = 2;
    pids_cgroup_charge(cg);  /* 1 */
    pids_cgroup_charge(cg);  /* 2 */
    ret = pids_cgroup_can_fork(cg);
    /* ret == -EAGAIN (达到上限) */
}
```

---

## 10.10 Phase 10 核心概念总结

| 概念 | 说明 |
|------|------|
| **Namespace** | 隔离"看到什么"— 7 种独立隔离维度 |
| **nsproxy** | 进程的 namespace 集合指针 |
| **PID ns** | 独立 PID 空间，容器 init = PID 1 |
| **UTS ns** | 独立主机名/域名 |
| **Mount ns** | 独立挂载表，pivot_root 切换根 |
| **User ns** | UID/GID 映射，非特权容器 |
| **cgroup v2** | 限制"能用多少"— 统一层级 |
| **Memory 控制器** | 内存上限，超限返回 ENOMEM |
| **PIDs 控制器** | 进程数上限，超限拒绝 fork |

**Phase 10 奠定的基础**：namespace + cgroup 实现了完整的容器隔离和资源限制。Phase 11 将为容器提供网络栈。
