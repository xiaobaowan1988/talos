# Phase 10：Linux Namespaces + cgroup v2

## 知识来源总览

- **容器隔离需求**：约 35%（7 种 namespace、clone flags）
- **cgroup v2 规范**：约 25%（统一层级、cgroupfs 接口、page_counter）
- **UNIX 进程模型**：约 15%（PID 语义、orphan reaping）
- **内核性能模式**：约 15%（percpu_ref、css_set 间接层）
- **前序依赖**：约 10%

## Namespace：让一个内核呈现多台机器假象

| Namespace | 隔离什么 | 不隔离的后果 |
|-----------|---------|-------------|
| PID | 进程 ID 空间 | 容器能 kill 宿主机进程 |
| Mount | 挂载点视图 | 容器看到宿主机文件 |
| Network | 网络设备/端口 | 容器端口冲突 |
| UTS | 主机名 | 所有容器共享 hostname |
| User | UID/GID 映射 | 容器 root = 宿主机 root |
| IPC | 消息队列/共享内存 | 容器间共享 IPC |
| Cgroup | cgroup 根视图 | 容器看到宿主机 cgroup |

### nsproxy —— namespace 打包

```c
struct nsproxy {
    atomic_t        count;
    struct uts_namespace    *uts_ns;
    struct ipc_namespace    *ipc_ns;
    struct mnt_namespace    *mnt_ns;
    struct pid_namespace    *pid_ns_for_children;
    struct net              *net_ns;
    struct cgroup_namespace *cgroup_ns;
};
```

共享全部 namespace 的进程共享同一个 nsproxy。只在 `clone(CLONE_NEW*)` 时创建新 nsproxy。

**`pid_ns_for_children`**（而非 `pid_ns`）：PID namespace 在 unshare 后只对子进程生效——如果当前进程的 PID 突然变了，所有依赖 PID 的代码全部失效。

### PID Namespace

```c
struct pid_namespace {
    struct pidmap       pidmap[PIDMAP_ENTRIES]; /* PID 位图 */
    int                 last_pid;               /* 顺序分配 */
    struct task_struct  *child_reaper;          /* 容器 init */
    unsigned int        level;                  /* 嵌套深度 */
    struct pid_namespace *parent;
};
```

**child_reaper**：容器的 PID 1。如果它退出，整个 namespace 的所有进程被强制杀死——这就是为什么容器 init 退出 = 容器销毁。

同一进程在不同 namespace 有不同 PID：宿主机看到 PID 1000，容器内看到 PID 1。

## cgroup v2：资源限制

### 统一层级

```
/sys/fs/cgroup/
├── cgroup.controllers
├── cgroup.subtree_control
└── container1/
    ├── cpu.max          # "100000 100000" = 100% CPU
    ├── memory.max       # "524288000" = 500MB
    ├── pids.max         # 最大进程数
    └── cgroup.procs     # 将 PID 写入此文件加入 cgroup
```

### Memory Controller

```c
struct mem_cgroup {
    struct cgroup_subsys_state css;
    struct page_counter memory;  /* 带上限的原子计数器 */
};

int mem_cgroup_charge(struct page *page, struct mm_struct *mm, gfp_t gfp) {
    struct mem_cgroup *memcg = get_mem_cgroup_from_mm(mm);

    if (page_counter_try_charge(&memcg->memory, 1, &counter)) {
        page->mem_cgroup = memcg;
        return 0;
    }

    /* 超限 → 尝试回收 → OOM kill */
    if (try_to_free_mem_cgroup_pages(memcg, gfp))
        goto retry;
    mem_cgroup_oom(memcg, gfp);
    return -ENOMEM;
}
```

**page_counter 层级检查**：子 cgroup 限制 512MB，父限制 1GB。子的使用量也算入父的使用量。

**与 Phase 2 的联动**：在 `alloc_pages()` 中插入 `mem_cgroup_charge()` 检查。超限时拒绝分配。

## 容器创建完整流程

```c
void create_container(void) {
    /* 1. 创建 cgroup 并设限 */
    mkdir("/sys/fs/cgroup/container1", 0755);
    write_file("memory.max", "512M");
    write_file("pids.max", "100");

    /* 2. clone 创建新进程 + 所有 namespace */
    int flags = CLONE_NEWNS | CLONE_NEWPID | CLONE_NEWNET |
                CLONE_NEWUTS | CLONE_NEWIPC | CLONE_NEWUSER;
    pid_t child = clone(container_init, stack_top, flags, NULL);

    /* 3. 将子进程加入 cgroup */
    write_file("cgroup.procs", pid_str);

    /* 子进程中：pivot_root + exec 容器程序 */
}
```

容器的本质：**namespace 隔离视图 + cgroup 限制资源**。不是一个独立的子系统，而是内核各处的小 hook 汇聚而成的隔离效果。
