# ARM64 v8 类Linux内核 — 从零实现学习计划

## 项目目标

从零实现一个基于 ARMv8-A（AArch64）的最小化类Linux内核，每个阶段均**严格参考 Linux 内核源码**中的对应实现（尤其是 `arch/arm64/` 下的 `.s`/`.S` 汇编文件）。

运行环境：**QEMU virt machine**（`-M virt -cpu cortex-a72 -m 1G`）

## 参考内核文件映射

| 模块 | 本项目文件 | 参考内核文件 |
|------|-----------|-------------|
| 启动入口 | `arch/arm64/kernel/head.S` | `arch/arm64/kernel/head.S` |
| 异常向量 | `arch/arm64/kernel/entry.S` | `arch/arm64/kernel/entry.S` |
| MMU 建立 | `arch/arm64/mm/mmu.c` | `arch/arm64/mm/mmu.c` |
| Buddy分配器 | `mm/page_alloc.c` | `mm/page_alloc.c` |
| GIC v3 | `drivers/irqchip/gic-v3.c` | `drivers/irqchip/irq-gic-v3.c` |
| 通用计时器 | `drivers/timer/arm_arch_timer.c` | `drivers/clocksource/arm_arch_timer.c` |
| CFS调度器 | `kernel/sched/fair.c` | `kernel/sched/fair.c` |
| 系统调用 | `arch/arm64/kernel/syscall.S` | `arch/arm64/kernel/syscall.c` + `entry.S` |
| ELF加载 | `kernel/syscall/exec.c` | `fs/binfmt_elf.c` |
| VirtIO MMIO | `drivers/virtio/virtio_mmio.c` | `drivers/virtio/virtio_mmio.c` |
| VFS核心 | `fs/vfs/super.c` | `fs/super.c`, `fs/inode.c` |
| dentry缓存 | `fs/vfs/dcache.c` | `fs/dcache.c` |
| squashfs | `fs/squashfs/` | `fs/squashfs/` |
| XFS | `fs/xfs/` | `fs/xfs/` |
| overlayfs | `fs/overlayfs/` | `fs/overlayfs/` |
| namespace | `kernel/namespace.c` | `kernel/nsproxy.c` |
| cgroup v2 | `kernel/cgroup/` | `kernel/cgroup/` |
| TCP/IP | `net/ipv4/` | `net/ipv4/` |
| netfilter | `net/netfilter/` | `net/netfilter/` |
| seccomp | `security/seccomp.c` | `kernel/seccomp.c` |
| eBPF JIT | `security/bpf/` | `arch/arm64/net/bpf_jit*` |
| Landlock | `security/landlock/` | `security/landlock/` |

## 目录结构

```
arm64os/
├── arch/arm64/
│   ├── kernel/
│   │   ├── head.S          # 启动入口，CPU初始化
│   │   ├── entry.S         # 异常向量表，中断入口
│   │   ├── syscall.S       # 系统调用分发
│   │   └── proc.S          # CPU特性检测
│   ├── mm/
│   │   ├── mmu.c           # MMU初始化，页表建立
│   │   ├── tlb.S           # TLB操作
│   │   └── cache.S         # Cache操作
│   └── include/
│       ├── asm/pgtable.h   # 页表项定义（PGD/PUD/PMD/PTE）
│       ├── asm/memory.h    # 虚拟地址空间布局
│       └── asm/sysreg.h    # 系统寄存器定义
├── mm/
│   ├── page_alloc.c        # Buddy系统物理内存分配
│   ├── slab.c              # Slab/Slub小对象分配器
│   └── vmalloc.c           # 内核虚拟地址分配
├── kernel/
│   ├── sched/
│   │   ├── core.c          # 调度器核心（context_switch）
│   │   ├── fair.c          # CFS完全公平调度
│   │   └── run_queue.c     # 运行队列管理
│   ├── irq/
│   │   ├── irqdesc.c       # IRQ描述符管理
│   │   └── handle.c        # 中断处理框架
│   └── syscall/
│       ├── sys_table.c     # 系统调用表
│       └── exec.c          # execve实现
├── drivers/
│   ├── irqchip/
│   │   └── gic-v3.c        # GIC v3中断控制器
│   ├── timer/
│   │   └── arm_arch_timer.c # ARMv8通用计时器
│   └── virtio/
│       ├── virtio_mmio.c   # VirtIO MMIO传输层
│       ├── virtio_blk.c    # VirtIO块设备
│       └── virtio_net.c    # VirtIO网络设备
├── fs/
│   ├── vfs/
│   │   ├── super.c         # 超级块操作
│   │   ├── inode.c         # inode管理
│   │   ├── dentry.c        # dentry缓存
│   │   └── file.c          # 文件操作
│   ├── squashfs/           # squashfs只读文件系统
│   ├── xfs/                # XFS日志结构文件系统
│   └── overlayfs/          # overlayfs联合挂载
├── net/
│   ├── core/               # 网络核心（sk_buff, socket）
│   ├── ipv4/               # TCP/IP协议栈
│   └── netfilter/          # netfilter钩子框架
├── security/
│   ├── seccomp.c           # seccomp系统调用过滤
│   ├── bpf/                # eBPF JIT（ARM64）
│   └── landlock/           # Landlock LSM
├── include/
│   ├── linux/              # 内核头文件
│   └── uapi/               # 用户态接口头文件
├── userspace/
│   ├── init/               # 最小init进程
│   └── libc/               # 最小C库（syscall封装）
├── scripts/
│   └── linker.ld           # 内核链接脚本
├── docs/phases/            # 各阶段详细设计文档
└── Makefile                # 构建系统
```

## 12阶段实施路线图

| 阶段 | 主题 | 核心内核文件参考 |
|------|------|----------------|
| Phase 1 | ARM64启动 + 异常向量 | `arch/arm64/kernel/head.S`, `entry.S` |
| Phase 2 | MMU + Buddy分配器 | `arch/arm64/mm/mmu.c`, `mm/page_alloc.c` |
| Phase 3 | GIC v3 + arch timer | `drivers/irqchip/irq-gic-v3.c` |
| Phase 4 | CFS调度器 | `kernel/sched/fair.c`, `core.c` |
| Phase 5 | 系统调用 + ELF加载 | `arch/arm64/kernel/entry.S`, `fs/binfmt_elf.c` |
| Phase 6 | VirtIO驱动 | `drivers/virtio/virtio_mmio.c` |
| Phase 7 | VFS四大对象 + dentry cache | `fs/super.c`, `fs/dcache.c` |
| Phase 8 | squashfs + XFS | `fs/squashfs/`, `fs/xfs/` |
| Phase 9 | overlayfs三层联合挂载 | `fs/overlayfs/` |
| Phase 10 | Namespaces + cgroup v2 | `kernel/nsproxy.c`, `kernel/cgroup/` |
| Phase 11 | TCP/IP + netfilter | `net/ipv4/tcp.c`, `net/netfilter/` |
| Phase 12 | seccomp + eBPF + Landlock | `kernel/seccomp.c`, `arch/arm64/net/bpf_jit*` |

详细设计见 `docs/phases/` 目录。
