# 前言

本书记录了一个 ARM64 操作系统内核的完整构建过程。从裸机启动的第一条汇编指令，到能够运行安全容器的完整内核，共 12 个阶段。

每一阶段的分析遵循同一原则：**追溯每一行代码的来源**。每一行代码都不是凭空写出的，它要么来自 ARM 架构手册的寄存器定义，要么来自 RFC 协议规范的字段描述，要么来自 Linux 内核的设计模式，要么来自 POSIX 标准的语义约束。

## 12 个阶段总览

| 阶段 | 主题 | 核心知识来源 |
|------|------|-------------|
| Phase 1 | 启动与异常向量 | ARM64 启动协议、EL2/EL1 |
| Phase 2 | MMU + Buddy 分配器 | ARM64 页表格式、伙伴算法 |
| Phase 3 | GIC v3 + Timer | ARM GIC 规范、Generic Timer |
| Phase 4 | CFS 调度器 | Linux CFS 设计、AAPCS64 |
| Phase 5 | 系统调用 + ELF 加载 | ARM64 SVC、ELF 规范 |
| Phase 6 | VirtIO 设备驱动 | VirtIO 规范 v1.1 |
| Phase 7 | VFS 四大对象 | Linux VFS 架构、POSIX |
| Phase 8 | squashfs + XFS | 磁盘格式规范、WAL 日志 |
| Phase 9 | overlayfs 联合挂载 | 容器存储模型、COW |
| Phase 10 | Namespace + cgroup v2 | 容器隔离机制 |
| Phase 11 | TCP/IP + netfilter | RFC 793/791、netfilter 框架 |
| Phase 12 | seccomp + eBPF + Landlock | BPF 指令集、ARM64 JIT |

## 阅读建议

- 每个阶段可独立阅读，但建议按顺序，因为后续阶段依赖前序概念
- 代码块中的注释标注了每一行的知识来源
- "为什么"比"是什么"更重要——理解设计决策比记住代码更有价值
