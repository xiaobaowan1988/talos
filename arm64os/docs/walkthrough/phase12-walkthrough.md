# Phase 12 Walkthrough: seccomp + eBPF JIT + Landlock LSM

> **目标**：实现 Linux 的三大安全机制 — 系统调用过滤、可编程内核、路径级沙箱。
> **最终效果**：seccomp 阻止非法 syscall，eBPF 程序 JIT 编译运行，Landlock 限制文件访问。

---

## 12.1 多层安全防御

```
  用户程序想访问文件
        │
        ▼
  ┌─────────────────┐
  │  Landlock LSM    │ ← 路径级别：你能访问 /etc 吗？
  │  (文件/网络沙箱) │
  └────────┬────────┘
           │ 通过
           ▼
  ┌─────────────────┐
  │  seccomp         │ ← 系统调用级别：你能调用 open() 吗？
  │  (BPF 过滤器)    │
  └────────┬────────┘
           │ 通过
           ▼
  ┌─────────────────┐
  │  内核执行 syscall │
  └────────┬────────┘
           │
           ▼
  ┌─────────────────┐
  │  cgroup          │ ← 资源级别：你还有内存配额吗？（Phase 10）
  └────────┬────────┘
           │
           ▼
  ┌─────────────────┐
  │  namespace       │ ← 隔离级别：你看到的是隔离视图（Phase 10）
  └─────────────────┘
```

---

## 12.2 seccomp — 系统调用过滤器

### 原理

seccomp 在每次系统调用**进入内核前**执行一段 BPF 程序，决定是否允许这个系统调用。

```
  用户态 SVC #0
      │
      ▼
  el0_svc → do_el0_svc()
      │
      ├─ seccomp_check(regs)     ← Phase 12 新增
      │   │
      │   ├─ 运行 BPF 过滤器
      │   ├─ 返回 ALLOW → 继续
      │   ├─ 返回 ERRNO → 设置错误码，不执行
      │   └─ 返回 KILL  → 杀死进程
      │
      └─ sys_call_table[nr](regs)
```

### 三种模式

```c
#define SECCOMP_MODE_DISABLED  0  /* 无过滤 */
#define SECCOMP_MODE_STRICT    1  /* 白名单：只允许 read/write/exit */
#define SECCOMP_MODE_FILTER    2  /* BPF 过滤器 */
```

### STRICT 模式

```c
int seccomp_check_strict(int syscall_nr)
{
    switch (syscall_nr) {
    case __NR_read:
    case __NR_write:
    case __NR_exit:
    case __NR_exit_group:
        return SECCOMP_RET_ALLOW;
    default:
        return SECCOMP_RET_KILL;  /* 其他全部杀死 */
    }
}
```

### FILTER 模式 — cBPF 程序

```c
struct sock_filter {
    uint16_t code;   /* 指令操作码 */
    uint8_t jt;      /* 条件为真时跳转偏移 */
    uint8_t jf;      /* 条件为假时跳转偏移 */
    uint32_t k;      /* 立即数 */
};
```

#### 示例：允许 write 和 exit，拒绝其他

```c
struct sock_filter filter[] = {
    /* [0] 加载 syscall 号到 A 寄存器 */
    BPF_STMT(BPF_LD | BPF_W | BPF_ABS, 0),
    /* offsetof(seccomp_data, nr) = 0 */

    /* [1] A == __NR_write (64)? 是→跳到[4], 否→继续 */
    BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, 64, 2, 0),

    /* [2] A == __NR_exit (93)? 是→跳到[4], 否→继续 */
    BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, 93, 1, 0),

    /* [3] 不允许 → 返回 ERRNO(EPERM) */
    BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | 1),

    /* [4] 允许 */
    BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
};
```

### cBPF 解释器

```c
uint32_t run_cbpf(struct sock_filter *prog, int prog_len,
                  struct seccomp_data *data)
{
    uint32_t A = 0, X = 0;   /* 累加器和索引寄存器 */
    uint32_t mem[16] = {0};   /* 暂存内存 */

    for (int pc = 0; pc < prog_len; pc++) {
        struct sock_filter *insn = &prog[pc];

        switch (insn->code) {
        case BPF_LD | BPF_W | BPF_ABS:
            /* 从 seccomp_data 加载 32 位字 */
            A = *(uint32_t *)((char *)data + insn->k);
            break;

        case BPF_JMP | BPF_JEQ | BPF_K:
            /* A == k? 跳 jt : 跳 jf */
            pc += (A == insn->k) ? insn->jt : insn->jf;
            break;

        case BPF_RET | BPF_K:
            return insn->k;  /* 返回判决 */

        /* ALU, ST, LDX 等其他指令... */
        }
    }
    return SECCOMP_RET_KILL;  /* 安全默认 */
}
```

---

## 12.3 eBPF — 可编程内核

### 从 cBPF 到 eBPF 的演进

| | cBPF (经典) | eBPF (扩展) |
|---|---|---|
| 寄存器 | 2 个 (A, X) | 11 个 (r0-r10) |
| 指令宽度 | 32 位 | 64 位 |
| 用途 | 包过滤 | 通用内核扩展 |
| JIT | 可选 | 标准 |

### eBPF 指令格式

```c
struct bpf_insn {
    uint8_t code;       /* 操作码 */
    uint8_t dst_reg:4;  /* 目标寄存器 */
    uint8_t src_reg:4;  /* 源寄存器 */
    int16_t off;        /* 偏移/跳转目标 */
    int32_t imm;        /* 立即数 */
};
```

### 寄存器映射（eBPF → ARM64）

```c
/* eBPF 寄存器 → AArch64 寄存器 */
static const int bpf2a64[] = {
    [BPF_REG_0]  = A64_R(7),   /* r0 → x7  (返回值) */
    [BPF_REG_1]  = A64_R(0),   /* r1 → x0  (参数1) */
    [BPF_REG_2]  = A64_R(1),   /* r2 → x1 */
    [BPF_REG_3]  = A64_R(2),   /* r3 → x2 */
    [BPF_REG_4]  = A64_R(3),   /* r4 → x3 */
    [BPF_REG_5]  = A64_R(4),   /* r5 → x4 */
    [BPF_REG_6]  = A64_R(19),  /* r6 → x19 (callee-saved) */
    [BPF_REG_7]  = A64_R(20),  /* r7 → x20 */
    [BPF_REG_8]  = A64_R(21),  /* r8 → x21 */
    [BPF_REG_9]  = A64_R(22),  /* r9 → x22 */
    [BPF_REG_10] = A64_R(25),  /* r10 → x25 (帧指针/栈) */
};
```

### eBPF 解释器

```c
uint64_t bpf_interpret(struct bpf_prog *prog, void *ctx)
{
    uint64_t regs[11] = {0};
    regs[BPF_REG_1] = (uint64_t)ctx;  /* 参数 */

    for (int pc = 0; pc < prog->len; pc++) {
        struct bpf_insn *insn = &prog->insns[pc];
        uint64_t *dst = &regs[insn->dst_reg];
        uint64_t src = (insn->code & BPF_X)
                     ? regs[insn->src_reg] : insn->imm;

        switch (insn->code) {
        case BPF_ALU64 | BPF_ADD | BPF_K:
            *dst += insn->imm;  break;
        case BPF_ALU64 | BPF_MOV | BPF_K:
            *dst = insn->imm;   break;

        case BPF_JMP | BPF_JEQ | BPF_K:
            if (*dst == (uint64_t)insn->imm)
                pc += insn->off;
            break;

        case BPF_JMP | BPF_EXIT:
            return regs[BPF_REG_0];  /* 返回 r0 */

        /* LDX, STX, 其他 ALU/JMP ... */
        }
    }
    return regs[BPF_REG_0];
}
```

### ARM64 JIT 编译器

JIT 将 eBPF 指令翻译为原生 ARM64 机器码，消除解释器开销：

```c
static int build_insn(struct bpf_insn *insn, struct jit_ctx *ctx)
{
    uint8_t dst = bpf2a64[insn->dst_reg];
    uint8_t src = bpf2a64[insn->src_reg];

    switch (insn->code) {
    case BPF_ALU64 | BPF_ADD | BPF_X:
        /* eBPF: dst += src */
        /* ARM64: ADD Xdst, Xdst, Xsrc */
        emit(A64_ADD(1, dst, dst, src), ctx);
        break;

    case BPF_ALU64 | BPF_MOV | BPF_K:
        /* eBPF: dst = imm32 */
        /* ARM64: MOVZ Xdst, #imm */
        emit(A64_MOVZ(1, dst, insn->imm, 0), ctx);
        break;

    case BPF_JMP | BPF_JEQ | BPF_K:
        /* eBPF: if (dst == imm) goto pc+off */
        /* ARM64: CMP Xdst, #imm; B.EQ target */
        emit(A64_CMP_IMM(1, dst, insn->imm), ctx);
        emit(A64_B_COND(A64_EQ, offset_to_target), ctx);
        break;

    case BPF_JMP | BPF_EXIT:
        /* ARM64: RET */
        emit(A64_RET(), ctx);
        break;
    }
    return 0;
}

/* JIT 编译完整流程 */
struct bpf_prog *bpf_jit_compile(struct bpf_prog *prog)
{
    struct jit_ctx ctx = {};

    /* Pass 1: 计算所有指令的偏移量 */
    for (int i = 0; i < prog->len; i++)
        build_insn(&prog->insns[i], &ctx);

    /* 分配可执行内存 */
    ctx.image = alloc_executable_pages(ctx.idx * 4);

    /* Pass 2: 生成实际机器码 */
    ctx.idx = 0;
    for (int i = 0; i < prog->len; i++)
        build_insn(&prog->insns[i], &ctx);

    prog->bpf_func = (void *)ctx.image;  /* 函数指针 */
    prog->jited = 1;

    return prog;
}
```

### eBPF 验证器

```c
int bpf_check(struct bpf_prog *prog)
{
    for (int i = 0; i < prog->len; i++) {
        struct bpf_insn *insn = &prog->insns[i];

        /* 检查 1: 寄存器范围 */
        if (insn->dst_reg > BPF_REG_10 ||
            insn->src_reg > BPF_REG_10)
            return -EINVAL;

        /* 检查 2: 跳转目标在程序范围内 */
        if (is_jump(insn->code)) {
            int target = i + 1 + insn->off;
            if (target < 0 || target >= prog->len)
                return -EINVAL;
        }

        /* 检查 3: 最后一条必须是 EXIT */
    }

    /* 检查 4: 不能有不可达代码、循环等 */
    return 0;
}
```

---

## 12.4 Landlock LSM — 路径级沙箱

### 原理

Landlock 让**非特权用户**也能限制自己的文件访问权限，且不可撤销。

### 使用流程

```
  1. 创建规则集（指定要控制哪些操作）
  2. 添加规则（指定允许哪些路径哪些操作）
  3. 激活沙箱（self_restrict，不可逆）
```

### 数据结构

```c
struct landlock_ruleset {
    uint64_t handled_access_fs;   /* 控制哪些文件操作 */
    uint64_t handled_access_net;  /* 控制哪些网络操作 */
    struct landlock_rule rules[16]; /* 规则列表 */
    int num_rules;
    int active;                    /* 是否已激活 */
};

struct landlock_rule {
    int type;                      /* LANDLOCK_RULE_PATH_BENEATH */
    char path[64];                 /* 允许访问的路径前缀 */
    uint64_t allowed_access;       /* 允许的操作位图 */
};
```

### 创建和配置

```c
int landlock_create_ruleset(uint64_t handled_access_fs,
                            uint64_t handled_access_net)
{
    struct landlock_ruleset *rs = alloc_ruleset();
    rs->handled_access_fs = handled_access_fs;
    rs->handled_access_net = handled_access_net;
    rs->num_rules = 0;
    rs->active = 0;
    return ruleset_to_fd(rs);
}

int landlock_add_rule(int ruleset_fd, int rule_type,
                      const char *path, uint64_t allowed_access)
{
    struct landlock_ruleset *rs = fd_to_ruleset(ruleset_fd);
    struct landlock_rule *rule = &rs->rules[rs->num_rules++];

    rule->type = rule_type;
    strcpy(rule->path, path);
    rule->allowed_access = allowed_access;

    return 0;
}
```

### 激活沙箱

```c
int landlock_restrict_self(int ruleset_fd)
{
    struct landlock_ruleset *rs = fd_to_ruleset(ruleset_fd);
    rs->active = 1;  /* 不可逆！ */

    /* 绑定到当前进程 */
    current_task->landlock_ruleset = rs;

    return 0;
}
```

### LSM 钩子检查

```c
int hook_file_open(struct inode *inode, const char *path)
{
    struct landlock_ruleset *rs = current_task->landlock_ruleset;
    if (!rs || !rs->active)
        return 0;  /* 没有沙箱，放行 */

    uint64_t requested = LANDLOCK_ACCESS_FS_READ_FILE;

    /* 检查路径是否匹配任何规则 */
    for (int i = 0; i < rs->num_rules; i++) {
        struct landlock_rule *rule = &rs->rules[i];

        /* 路径前缀匹配 */
        if (path_starts_with(path, rule->path)) {
            /* 检查请求的操作是否被允许 */
            if ((rule->allowed_access & requested) == requested)
                return 0;  /* 允许 */
        }
    }

    /* handled_access_fs 中声明了控制此操作，但没有规则允许 */
    if (rs->handled_access_fs & requested)
        return -EACCES;  /* 拒绝！ */

    return 0;
}
```

### 文件访问权限位

```c
#define LANDLOCK_ACCESS_FS_EXECUTE      (1ULL << 0)
#define LANDLOCK_ACCESS_FS_WRITE_FILE   (1ULL << 1)
#define LANDLOCK_ACCESS_FS_READ_FILE    (1ULL << 2)
#define LANDLOCK_ACCESS_FS_READ_DIR     (1ULL << 3)
#define LANDLOCK_ACCESS_FS_REMOVE_DIR   (1ULL << 4)
#define LANDLOCK_ACCESS_FS_REMOVE_FILE  (1ULL << 5)
#define LANDLOCK_ACCESS_FS_MAKE_REG     (1ULL << 6)
#define LANDLOCK_ACCESS_FS_MAKE_DIR     (1ULL << 7)
```

---

## 12.5 测试验证

```c
static void test_phase12(void)
{
    /* === seccomp 测试 === */

    /* STRICT 模式：只允许 read/write/exit */
    current_task->seccomp.mode = SECCOMP_MODE_STRICT;
    int ret = seccomp_check(current_task, __NR_open);
    /* ret == SECCOMP_RET_KILL → open 被禁止 */

    ret = seccomp_check(current_task, __NR_write);
    /* ret == SECCOMP_RET_ALLOW → write 被允许 */

    /* FILTER 模式：BPF 过滤器 */
    /* 安装允许 write+exit 的过滤器 */
    /* 验证 open → EPERM */

    /* === eBPF 测试 === */

    /* 加载简单程序: r0 = r1 + 42; exit */
    struct bpf_insn prog[] = {
        BPF_MOV64_REG(BPF_REG_0, BPF_REG_1),  /* r0 = r1 */
        BPF_ALU64_IMM(BPF_ADD, BPF_REG_0, 42), /* r0 += 42 */
        BPF_EXIT_INSN(),                         /* return r0 */
    };

    /* 验证器检查 */
    bpf_check(&prog);  /* 通过 */

    /* 解释器执行 */
    uint64_t result = bpf_interpret(&prog, (void *)100);
    /* result == 142 */

    /* JIT 编译并执行 */
    bpf_jit_compile(&prog);
    result = prog.bpf_func((void *)100);
    /* result == 142 (原生速度！) */

    /* === Landlock 测试 === */

    /* 创建规则集：控制文件读取 */
    int rs_fd = landlock_create_ruleset(
        LANDLOCK_ACCESS_FS_READ_FILE, 0);

    /* 添加规则：允许读取 /sq/ 下的文件 */
    landlock_add_rule(rs_fd, LANDLOCK_RULE_PATH_BENEATH,
                      "/sq", LANDLOCK_ACCESS_FS_READ_FILE);

    /* 激活沙箱 */
    landlock_restrict_self(rs_fd);

    /* 验证 */
    ret = hook_file_open(inode, "/sq/hello.txt");
    /* ret == 0 → 允许（有规则覆盖） */

    ret = hook_file_open(inode, "/xfs/secret.txt");
    /* ret == -EACCES → 拒绝（没有规则覆盖） */
}
```

---

## 12.6 Phase 12 核心概念总结

| 概念 | 说明 |
|------|------|
| **seccomp** | 系统调用过滤器，每次 syscall 前检查 |
| **cBPF** | 经典 BPF，2 寄存器，用于 seccomp 过滤 |
| **eBPF** | 扩展 BPF，11 寄存器，通用内核编程 |
| **JIT** | 将 eBPF 编译为 ARM64 机器码 |
| **验证器** | 确保 eBPF 程序安全（无越界/无循环） |
| **Landlock** | 非特权路径级沙箱，不可逆激活 |
| **LSM 钩子** | Linux Security Module 在文件操作前检查 |

---

## 12.7 完整安全层次

```
  用户请求 open("/etc/passwd")
      │
      ▼
  ① Landlock: /etc 在规则集中吗？
      │  没有规则 → -EACCES ✗
      │  有规则且允许 READ → 继续
      ▼
  ② seccomp: open() 在 BPF 过滤器允许列表中吗？
      │  不在 → SECCOMP_RET_ERRNO ✗
      │  在 → 继续
      ▼
  ③ 内核执行 sys_open()
      │
      ▼
  ④ cgroup: 内存/进程数配额够吗？
      │  不够 → -ENOMEM ✗
      │  够 → 继续
      ▼
  ⑤ namespace: 看到的是隔离后的文件系统视图
      │
      ▼
  ⑥ VFS → 具体文件系统 → 磁盘
```

**Phase 12 完成了整个内核**：从裸机启动（Phase 1）到安全沙箱（Phase 12），一个完整的类 Linux 内核。

---

## 12.8 完整源码清单

> Phase 12 在 Phase 11 基础上新增 11 个源文件（3 个头文件 + 8 个 C 文件），修改 `kernel/main.c` 和 `Makefile`。实现 seccomp 系统调用过滤、eBPF 解释器/验证器/JIT 编译器、Landlock LSM 文件系统与网络沙箱。

### 新增目录结构

```
arm64os/
├── arch/arm64/
│   ├── include/asm/
│   │   ├── memory.h          (Phase 2, 不变)
│   │   ├── pgtable.h         (Phase 2, 不变)
│   │   └── sysreg.h          (Phase 2, 不变)
│   ├── kernel/
│   │   ├── head.S             (Phase 1, 不变)
│   │   ├── entry.S            (Phase 5, 不变)
│   │   └── process.o          (Phase 4, 不变)
│   ├── mm/
│   │   ├── mmu.c              (Phase 2, 不变)
│   │   ├── proc.S             (Phase 2, 不变)
│   │   └── tlb.S              (Phase 2, 不变)
│   └── net/
│       └── bpf_jit_comp.c     ← 新增（ARM64 eBPF JIT 编译器）
├── drivers/
│   ├── block/
│   │   └── virtio_blk.c       (Phase 6, 不变)
│   ├── irqchip/
│   │   └── gic-v3.c           (Phase 3, 不变)
│   ├── net/
│   │   └── virtio_net.c       (Phase 6, 不变)
│   ├── timer/
│   │   └── arm_arch_timer.c   (Phase 3, 不变)
│   └── virtio/
│       ├── virtio.c            (Phase 6, 不变)
│       ├── virtio_mmio.c       (Phase 6, 不变)
│       └── virtio_ring.c       (Phase 6, 不变)
├── fs/
│   ├── overlayfs/              (Phase 9, 不变)
│   ├── ramfs/                  (Phase 7, 不变)
│   ├── squashfs/               (Phase 8, 不变)
│   ├── vfs/                    (Phase 7, 不变)
│   ├── xfs/                    (Phase 8, 不变)
│   ├── binfmt_elf.c            (Phase 5, 不变)
│   └── mount.c                 (Phase 10, 不变)
├── include/linux/
│   ├── bpf.h                  ← 新增（eBPF 指令集与数据结构）
│   ├── landlock.h             ← 新增（Landlock LSM 接口）
│   ├── seccomp.h              ← 新增（seccomp + cBPF 指令集）
│   ├── io.h                    (Phase 2, 不变)
│   ├── irq.h                   (Phase 3, 不变)
│   ├── list.h                  (Phase 2, 不变)
│   ├── sched.h                 (Phase 4, 不变)
│   └── types.h                 (Phase 1, 不变)
├── kernel/
│   ├── bpf/
│   │   ├── core.c             ← 新增（eBPF 解释器与执行入口）
│   │   ├── syscall.c          ← 新增（bpf() 系统调用）
│   │   └── verifier.c         ← 新增（eBPF 验证器）
│   ├── cgroup/                 (Phase 10, 不变)
│   ├── irq/                    (Phase 3, 不变)
│   ├── sched/                  (Phase 4, 不变)
│   ├── syscall/                (Phase 5, 不变)
│   ├── fork.c                  (Phase 4, 不变)
│   ├── main.c                 ← 修改（Phase 12 初始化 + test_phase12）
│   ├── nsproxy.c               (Phase 10, 不变)
│   ├── pid_namespace.c         (Phase 10, 不变)
│   ├── printk.c                (Phase 1, 不变)
│   ├── seccomp.c              ← 新增（seccomp 过滤框架 + cBPF 解释器）
│   ├── user_namespace.c        (Phase 10, 不变)
│   └── utsname.c               (Phase 10, 不变)
├── lib/
│   └── rbtree.c                (Phase 4, 不变)
├── mm/
│   ├── memblock.c              (Phase 2, 不变)
│   └── page_alloc.c            (Phase 2, 不变)
├── net/
│   ├── core/                   (Phase 11, 不变)
│   ├── ipv4/                   (Phase 11, 不变)
│   └── netfilter/              (Phase 11, 不变)
├── security/
│   └── landlock/
│       ├── fs.c               ← 新增（Landlock 文件系统访问控制）
│       ├── net.c              ← 新增（Landlock 网络访问控制）
│       └── ruleset.c          ← 新增（Landlock 规则集管理）
├── scripts/linker.ld           (Phase 1, 不变)
└── Makefile                   ← 修改（Phase 12 构建规则）
```

### 新增文件 1: `include/linux/seccomp.h`

```c
/* SPDX-License-Identifier: GPL-2.0 */
/*
 * arm64os/include/linux/seccomp.h
 *
 * seccomp (secure computing) 系统调用过滤
 *
 * 参考：include/uapi/linux/seccomp.h
 *       include/linux/seccomp.h
 *       include/uapi/linux/filter.h
 *
 * Phase 12 实现：
 *   - seccomp_data：每次系统调用传递给 BPF 过滤器的上下文
 *   - seccomp_filter：BPF 过滤器链（可叠加）
 *   - Classic BPF (cBPF) 指令集：sock_filter + sock_fprog
 *   - seccomp 返回值动作码
 */

#ifndef __LINUX_SECCOMP_H
#define __LINUX_SECCOMP_H

#include <linux/types.h>

/* ============================================================
 * seccomp 模式
 * ============================================================ */
#define SECCOMP_MODE_DISABLED   0   /* seccomp 未启用 */
#define SECCOMP_MODE_STRICT     1   /* 只允许 read/write/exit/sigreturn */
#define SECCOMP_MODE_FILTER     2   /* BPF 过滤器模式 */

/* ============================================================
 * seccomp 返回值（BPF 程序返回）
 *
 * 高16位 = 动作码
 * 低16位 = 动作数据（如 ERRNO 的错误码）
 * ============================================================ */
#define SECCOMP_RET_ACTION_FULL 0xffff0000U
#define SECCOMP_RET_DATA        0x0000ffffU

#define SECCOMP_RET_KILL_PROCESS 0x80000000U  /* 杀死整个进程 */
#define SECCOMP_RET_KILL_THREAD  0x00000000U  /* 杀死当前线程 */
#define SECCOMP_RET_TRAP         0x00030000U  /* 发送 SIGSYS */
#define SECCOMP_RET_ERRNO        0x00050000U  /* 返回指定错误码 */
#define SECCOMP_RET_TRACE        0x7ff00000U  /* 通知 ptrace */
#define SECCOMP_RET_ALLOW        0x7fff0000U  /* 允许执行 */

/* ============================================================
 * seccomp_data - BPF 过滤器的输入数据
 *
 * 每次系统调用时由内核填充，传给 BPF 程序。
 *
 * 参考：include/uapi/linux/seccomp.h struct seccomp_data
 * ============================================================ */
struct seccomp_data {
    int     nr;             /* 系统调用号 */
    u32     arch;           /* AUDIT_ARCH_* 值 */
    u64     instruction_pointer;
    u64     args[6];        /* 系统调用参数 x0-x5 */
};

/* ARM64 audit 架构标识 */
#define AUDIT_ARCH_AARCH64  0xC00000B7

/* ============================================================
 * Classic BPF (cBPF) 指令集
 *
 * seccomp 使用经典 BPF（非 eBPF）来过滤系统调用。
 *
 * 参考：include/uapi/linux/filter.h
 * ============================================================ */

/* BPF 指令结构 */
struct sock_filter {
    u16     code;       /* 操作码 */
    u8      jt;         /* 条件为真时跳转偏移 */
    u8      jf;         /* 条件为假时跳转偏移 */
    u32     k;          /* 立即数/内存偏移 */
};

/* BPF 程序 */
struct sock_fprog {
    u16                     len;        /* 指令数 */
    struct sock_filter     *filter;     /* 指令数组 */
};

/* BPF 指令类别（code 字段的高3位）*/
#define BPF_CLASS(code) ((code) & 0x07)
#define BPF_LD      0x00    /* 加载到累加器 A */
#define BPF_LDX     0x01    /* 加载到索引寄存器 X */
#define BPF_ST      0x02    /* 存储 A 到暂存区 */
#define BPF_STX     0x03    /* 存储 X 到暂存区 */
#define BPF_ALU     0x04    /* ALU 运算 */
#define BPF_JMP     0x05    /* 跳转 */
#define BPF_RET     0x06    /* 返回 */
#define BPF_MISC    0x07    /* 杂项（TAX/TXA）*/

/* 大小修饰符（code 字段的 bit 3-4）*/
#define BPF_SIZE(code)  ((code) & 0x18)
#define BPF_W       0x00    /* 32位 word */
#define BPF_H       0x08    /* 16位 half-word */
#define BPF_B       0x10    /* 8位 byte */

/* 寻址模式（code 字段的 bit 5-7）*/
#define BPF_MODE(code)  ((code) & 0xe0)
#define BPF_IMM     0x00    /* 立即数 */
#define BPF_ABS     0x20    /* 绝对地址（数据包偏移）*/
#define BPF_IND     0x40    /* 间接地址 */
#define BPF_MEM     0x60    /* 暂存区内存 */

/* ALU/JMP 操作码（code 字段的 bit 4-7）*/
#define BPF_OP(code)    ((code) & 0xf0)
#define BPF_ADD     0x00
#define BPF_SUB     0x10
#define BPF_MUL     0x20
#define BPF_DIV     0x30
#define BPF_OR      0x40
#define BPF_AND     0x50
#define BPF_LSH     0x60
#define BPF_RSH     0x70
#define BPF_NEG     0x80
#define BPF_JA      0x00    /* 无条件跳转 */
#define BPF_JEQ     0x10    /* == */
#define BPF_JGT     0x20    /* > (unsigned) */
#define BPF_JGE     0x30    /* >= (unsigned) */
#define BPF_JSET    0x40    /* & (位测试) */

/* 源操作数选择 */
#define BPF_SRC(code)   ((code) & 0x08)
#define BPF_K       0x00    /* 使用立即数 k */
#define BPF_X       0x08    /* 使用索引寄存器 X */

/* 返回值来源 */
#define BPF_RVAL(code)  ((code) & 0x18)
#define BPF_A       0x10    /* 返回累加器 A */

/* TAX / TXA */
#define BPF_TAX     0x00    /* A = X */
#define BPF_TXA     0x80    /* X = A */

/* ============================================================
 * BPF 指令构造宏（用户态构建 seccomp 过滤器）
 * ============================================================ */

/* 语句（无跳转）*/
#define BPF_STMT(code, k) \
    ((struct sock_filter){ (u16)(code), 0, 0, (u32)(k) })

/* 跳转指令 */
#define BPF_JUMP(code, k, jt, jf) \
    ((struct sock_filter){ (u16)(code), (u8)(jt), (u8)(jf), (u32)(k) })

/* BPF 暂存区大小（M[0..15]）*/
#define BPF_MEMWORDS    16

/* ============================================================
 * seccomp_filter - 内核 seccomp 过滤器结构
 *
 * 过滤器链：每次 prctl(SECCOMP_MODE_FILTER) 添加一个新过滤器，
 * prev 指向上一个。执行时从最新到最旧遍历，取最严格的返回值。
 *
 * 参考：kernel/seccomp.c struct seccomp_filter
 * ============================================================ */

/* 单个 BPF 程序（编译后的指令数组）*/
struct bpf_prog {
    u32                     len;        /* 指令条数 */
    struct sock_filter     *insns;      /* 指令数组指针 */
};

/* seccomp 过滤器（可链式叠加）*/
struct seccomp_filter {
    int                         refcount;   /* 引用计数 */
    bool                        log;        /* 是否记录日志 */
    struct seccomp_filter      *prev;       /* 前一个过滤器（链表）*/
    struct bpf_prog             prog;       /* BPF 程序 */
};

/* 过滤器池大小 */
#define MAX_SECCOMP_FILTERS 8

/* ============================================================
 * seccomp 进程状态（嵌入 task_struct）
 * ============================================================ */
struct seccomp {
    int                     mode;       /* SECCOMP_MODE_* */
    struct seccomp_filter  *filter;     /* 过滤器链头 */
};

/* ============================================================
 * seccomp 接口函数
 * ============================================================ */

/* 初始化 seccomp 子系统（过滤器池清零）*/
void seccomp_init(void);

/* 安装 STRICT 模式（只允许 read/write/exit/exit_group）*/
int seccomp_set_mode_strict(void);

/* 安装 FILTER 模式（BPF 过滤器）*/
int seccomp_set_mode_filter(struct sock_fprog *fprog);

/* 系统调用前检查（由 do_el0_svc 调用）*/
int __secure_computing(const struct seccomp_data *sd);

/* ARM64 系统调用号定义（用于 STRICT 模式白名单）*/
#define __NR_read       63
#define __NR_write      64
#define __NR_exit       93
#define __NR_exit_group 94

/* 错误码 */
#ifndef EPERM
#define EPERM   1
#endif
#ifndef EACCES
#define EACCES  13
#endif
#ifndef EINVAL
#define EINVAL  22
#endif
#ifndef ENOSYS
#define ENOSYS  38
#endif

/* prctl 命令 */
#define PR_SET_NO_NEW_PRIVS 38
#define PR_SET_SECCOMP      22

/* offsetof 宏 */
#ifndef offsetof
#define offsetof(TYPE, MEMBER) ((size_t)&((TYPE *)0)->MEMBER)
#endif

#endif /* __LINUX_SECCOMP_H */
```

### 新增文件 2: `include/linux/bpf.h`

```c
/* SPDX-License-Identifier: GPL-2.0 */
/*
 * arm64os/include/linux/bpf.h
 *
 * eBPF (extended Berkeley Packet Filter) 数据结构与指令集
 *
 * 参考：include/linux/bpf.h
 *       include/uapi/linux/bpf.h
 *       include/linux/filter.h
 *
 * Phase 12 实现：
 *   - eBPF 64位 RISC 指令集（11 个寄存器 r0-r10）
 *   - bpf_insn：eBPF 指令结构
 *   - ebpf_prog：eBPF 程序容器
 *   - 解释器、验证器、JIT 编译器接口
 */

#ifndef __LINUX_BPF_H
#define __LINUX_BPF_H

#include <linux/types.h>

/* ============================================================
 * eBPF 寄存器编号
 *
 * r0  = 返回值 / helper 函数返回值
 * r1-r5 = 函数调用参数
 * r6-r9 = 被调用者保存寄存器（callee-saved）
 * r10 = 只读栈帧指针（frame pointer）
 * ============================================================ */
#define BPF_REG_0   0
#define BPF_REG_1   1
#define BPF_REG_2   2
#define BPF_REG_3   3
#define BPF_REG_4   4
#define BPF_REG_5   5
#define BPF_REG_6   6
#define BPF_REG_7   7
#define BPF_REG_8   8
#define BPF_REG_9   9
#define BPF_REG_10  10
#define MAX_BPF_REG 11

/* ============================================================
 * eBPF 指令编码
 *
 * code 字段编码：
 *   bit 0-2: 指令类别（BPF_LD, BPF_ALU64, BPF_JMP, ...）
 *   bit 3:   源操作数（BPF_K=立即数, BPF_X=寄存器）
 *   bit 4-7: 操作码（ADD, SUB, MOV, ...）
 *
 * 参考：include/uapi/linux/bpf_common.h
 * ============================================================ */

/* 指令类别（扩展 cBPF 到 64 位）*/
#define EBPF_CLS_LD      0x00
#define EBPF_CLS_LDX     0x01
#define EBPF_CLS_ST      0x02
#define EBPF_CLS_STX     0x03
#define EBPF_CLS_ALU     0x04
#define EBPF_CLS_JMP     0x05
#define EBPF_CLS_RET     0x06    /* cBPF 兼容 */
#define EBPF_CLS_ALU64   0x07    /* eBPF 64位 ALU */

/* 大小修饰符 */
#define EBPF_DW     0x18    /* 64位 double-word */
/* BPF_W(0x00), BPF_H(0x08), BPF_B(0x10) 从 seccomp.h 继承 */

/* 寻址模式 */
#define EBPF_MEM    0x60    /* 内存操作 */

/* ALU 操作码 */
#define EBPF_ADD    0x00
#define EBPF_SUB    0x10
#define EBPF_MUL    0x20
#define EBPF_DIV    0x30
#define EBPF_OR     0x40
#define EBPF_AND    0x50
#define EBPF_LSH    0x60
#define EBPF_RSH    0x70
#define EBPF_NEG    0x80
#define EBPF_MOD    0x90
#define EBPF_XOR    0xa0
#define EBPF_MOV    0xb0
#define EBPF_ARSH   0xc0    /* 算术右移 */

/* JMP 操作码 */
#define EBPF_JA     0x00    /* 无条件跳转 */
#define EBPF_JEQ    0x10    /* == */
#define EBPF_JGT    0x20    /* > (unsigned) */
#define EBPF_JGE    0x30    /* >= (unsigned) */
#define EBPF_JSET   0x40    /* 位测试 */
#define EBPF_JNE    0x50    /* != */
#define EBPF_JSGT   0x60    /* > (signed) */
#define EBPF_JSGE   0x70    /* >= (signed) */
#define EBPF_JLT    0xa0    /* < (unsigned) */
#define EBPF_JLE    0xb0    /* <= (unsigned) */
#define EBPF_CALL   0x80    /* 函数调用 */
#define EBPF_EXIT   0x90    /* 退出程序 */

/* 源操作数 */
#define EBPF_K      0x00    /* 立即数 */
#define EBPF_X      0x08    /* 寄存器 */

/* ============================================================
 * bpf_insn - eBPF 指令结构（64位 RISC）
 *
 * 每条指令 8 字节：
 *   code(8) | dst_reg(4):src_reg(4) | off(16) | imm(32)
 *
 * 参考：include/uapi/linux/bpf.h struct bpf_insn
 * ============================================================ */
struct bpf_insn {
    u8      code;           /* 操作码 */
    u8      dst_reg:4;      /* 目标寄存器 */
    u8      src_reg:4;      /* 源寄存器 */
    s16     off;            /* 偏移量（内存访问 / 跳转目标）*/
    s32     imm;            /* 立即数 */
};

/* ============================================================
 * eBPF 指令构造宏
 * ============================================================ */

/* ALU64 指令（64位）*/
#define BPF_ALU64_IMM(OP, DST, IMM) \
    ((struct bpf_insn){ .code = EBPF_CLS_ALU64 | (OP) | EBPF_K, \
                        .dst_reg = (DST), .src_reg = 0, \
                        .off = 0, .imm = (IMM) })

#define BPF_ALU64_REG(OP, DST, SRC) \
    ((struct bpf_insn){ .code = EBPF_CLS_ALU64 | (OP) | EBPF_X, \
                        .dst_reg = (DST), .src_reg = (SRC), \
                        .off = 0, .imm = 0 })

/* MOV 指令 */
#define BPF_MOV64_IMM(DST, IMM) BPF_ALU64_IMM(EBPF_MOV, DST, IMM)
#define BPF_MOV64_REG(DST, SRC) BPF_ALU64_REG(EBPF_MOV, DST, SRC)

/* 内存加载 */
#define BPF_LDX_MEM(SIZE, DST, SRC, OFF) \
    ((struct bpf_insn){ .code = EBPF_CLS_LDX | (SIZE) | EBPF_MEM, \
                        .dst_reg = (DST), .src_reg = (SRC), \
                        .off = (OFF), .imm = 0 })

/* 内存存储（寄存器）*/
#define BPF_STX_MEM(SIZE, DST, SRC, OFF) \
    ((struct bpf_insn){ .code = EBPF_CLS_STX | (SIZE) | EBPF_MEM, \
                        .dst_reg = (DST), .src_reg = (SRC), \
                        .off = (OFF), .imm = 0 })

/* 内存存储（立即数）*/
#define BPF_ST_MEM(SIZE, DST, OFF, IMM) \
    ((struct bpf_insn){ .code = EBPF_CLS_ST | (SIZE) | EBPF_MEM, \
                        .dst_reg = (DST), .src_reg = 0, \
                        .off = (OFF), .imm = (IMM) })

/* 跳转指令（立即数）*/
#define BPF_JMP_IMM(OP, DST, IMM, OFF) \
    ((struct bpf_insn){ .code = EBPF_CLS_JMP | (OP) | EBPF_K, \
                        .dst_reg = (DST), .src_reg = 0, \
                        .off = (OFF), .imm = (IMM) })

/* 跳转指令（寄存器）*/
#define BPF_JMP_REG(OP, DST, SRC, OFF) \
    ((struct bpf_insn){ .code = EBPF_CLS_JMP | (OP) | EBPF_X, \
                        .dst_reg = (DST), .src_reg = (SRC), \
                        .off = (OFF), .imm = 0 })

/* 退出指令 */
#define BPF_EXIT_INSN() \
    ((struct bpf_insn){ .code = EBPF_CLS_JMP | EBPF_EXIT, \
                        .dst_reg = 0, .src_reg = 0, \
                        .off = 0, .imm = 0 })

/* 函数调用 */
#define BPF_CALL_INSN(FUNC_ID) \
    ((struct bpf_insn){ .code = EBPF_CLS_JMP | EBPF_CALL, \
                        .dst_reg = 0, .src_reg = 0, \
                        .off = 0, .imm = (FUNC_ID) })

/* ============================================================
 * eBPF 程序类型
 * ============================================================ */
enum bpf_prog_type {
    BPF_PROG_TYPE_UNSPEC = 0,
    BPF_PROG_TYPE_SOCKET_FILTER,
    BPF_PROG_TYPE_KPROBE,
    BPF_PROG_TYPE_SCHED_CLS,
    BPF_PROG_TYPE_TRACEPOINT,
    BPF_PROG_TYPE_XDP,
};

/* ============================================================
 * bpf() 系统调用命令
 * ============================================================ */
enum bpf_cmd {
    BPF_PROG_LOAD = 5,
    BPF_PROG_RUN  = 31,    /* BPF_PROG_TEST_RUN */
};

/* bpf() 系统调用属性 */
struct bpf_attr {
    /* BPF_PROG_LOAD */
    u32     prog_type;
    u32     insn_cnt;
    struct bpf_insn *insns;     /* 指令数组 */

    /* BPF_PROG_RUN */
    u32     prog_fd;            /* 程序 fd（返回值/索引）*/
    void   *data_in;            /* 测试输入数据 */
    u32     data_size_in;
    u32     retval;             /* 返回值（输出）*/
};

/* ============================================================
 * ebpf_prog - 内核 eBPF 程序结构
 *
 * 包含原始 eBPF 指令、JIT 编译结果和验证状态。
 * ============================================================ */

/* eBPF 程序最大指令数 */
#define BPF_MAXINSNS    128

/* JIT 编译后的机器码最大字节数 */
#define BPF_JIT_MAX_SIZE 2048

/* eBPF 程序池大小 */
#define MAX_EBPF_PROGS  8

/* eBPF 栈大小 */
#define BPF_STACK_SIZE  512

struct ebpf_prog {
    int                 used;           /* 是否已分配 */
    enum bpf_prog_type  type;           /* 程序类型 */
    u32                 len;            /* 指令条数 */
    struct bpf_insn     insns[BPF_MAXINSNS]; /* 指令数组 */
    bool                jit_done;       /* 是否已 JIT 编译 */
    void               *jit_image;      /* JIT 机器码指针 */
    u32                 jit_size;       /* JIT 机器码大小 */
};

/* ============================================================
 * eBPF 接口函数
 * ============================================================ */

/* 初始化 eBPF 子系统 */
void bpf_init(void);

/* 解释器执行 eBPF 程序 */
u64 bpf_prog_run_interp(const struct ebpf_prog *prog, const void *ctx);

/* 验证 eBPF 程序字节码安全性 */
int bpf_verify(const struct ebpf_prog *prog);

/* ARM64 JIT 编译 */
int bpf_jit_compile(struct ebpf_prog *prog);

/* 运行 eBPF 程序（自动选择 JIT 或解释器）*/
u64 bpf_prog_run(const struct ebpf_prog *prog, const void *ctx);

/* bpf() 系统调用处理 */
int sys_bpf(int cmd, struct bpf_attr *attr, u32 size);

/* 根据 fd（索引）查找程序 */
struct ebpf_prog *bpf_prog_get(int fd);

/* JIT 全局开关 */
extern int bpf_jit_enable;

#endif /* __LINUX_BPF_H */
```

### 新增文件 3: `include/linux/landlock.h`

```c
/* SPDX-License-Identifier: GPL-2.0 */
/*
 * arm64os/include/linux/landlock.h
 *
 * Landlock LSM（Linux Security Module）— 不可权限提升的沙箱机制
 *
 * 参考：include/uapi/linux/landlock.h
 *       security/landlock/ruleset.h
 *       security/landlock/fs.h
 *
 * Phase 12 实现：
 *   - Landlock 规则集（ruleset）：声明要限制的访问类型
 *   - Landlock 规则（rule）：路径 → 允许的权限
 *   - 文件系统访问控制（hook_file_open）
 *   - 网络访问控制（hook_socket_connect）
 *   - 三个系统调用：create_ruleset, add_rule, restrict_self
 *
 * 设计原则：
 *   1. 无需 root 权限（普通进程可自我沙箱化）
 *   2. 不可绕过（子进程继承且无法放松规则）
 *   3. 规则叠加（只能更严，不能更松）
 *   4. 基于路径层级访问控制
 */

#ifndef __LINUX_LANDLOCK_H
#define __LINUX_LANDLOCK_H

#include <linux/types.h>

/* ============================================================
 * Landlock 文件系统访问权限位
 *
 * 参考：include/uapi/linux/landlock.h
 * ============================================================ */
#define LANDLOCK_ACCESS_FS_EXECUTE          (1ULL << 0)
#define LANDLOCK_ACCESS_FS_WRITE_FILE       (1ULL << 1)
#define LANDLOCK_ACCESS_FS_READ_FILE        (1ULL << 2)
#define LANDLOCK_ACCESS_FS_READ_DIR         (1ULL << 3)
#define LANDLOCK_ACCESS_FS_REMOVE_DIR       (1ULL << 4)
#define LANDLOCK_ACCESS_FS_REMOVE_FILE      (1ULL << 5)
#define LANDLOCK_ACCESS_FS_MAKE_CHAR        (1ULL << 6)
#define LANDLOCK_ACCESS_FS_MAKE_DIR         (1ULL << 7)
#define LANDLOCK_ACCESS_FS_MAKE_REG         (1ULL << 8)
#define LANDLOCK_ACCESS_FS_MAKE_SOCK        (1ULL << 9)
#define LANDLOCK_ACCESS_FS_MAKE_FIFO        (1ULL << 10)
#define LANDLOCK_ACCESS_FS_MAKE_BLOCK       (1ULL << 11)
#define LANDLOCK_ACCESS_FS_MAKE_SYM         (1ULL << 12)
#define LANDLOCK_ACCESS_FS_REFER            (1ULL << 13)
#define LANDLOCK_ACCESS_FS_TRUNCATE         (1ULL << 14)

/* 所有文件系统访问权限的联合 */
#define LANDLOCK_ACCESS_FS_ALL  ( \
    LANDLOCK_ACCESS_FS_EXECUTE      | \
    LANDLOCK_ACCESS_FS_WRITE_FILE   | \
    LANDLOCK_ACCESS_FS_READ_FILE    | \
    LANDLOCK_ACCESS_FS_READ_DIR     | \
    LANDLOCK_ACCESS_FS_REMOVE_DIR   | \
    LANDLOCK_ACCESS_FS_REMOVE_FILE  | \
    LANDLOCK_ACCESS_FS_MAKE_CHAR    | \
    LANDLOCK_ACCESS_FS_MAKE_DIR     | \
    LANDLOCK_ACCESS_FS_MAKE_REG     | \
    LANDLOCK_ACCESS_FS_MAKE_SOCK    | \
    LANDLOCK_ACCESS_FS_MAKE_FIFO    | \
    LANDLOCK_ACCESS_FS_MAKE_BLOCK   | \
    LANDLOCK_ACCESS_FS_MAKE_SYM     | \
    LANDLOCK_ACCESS_FS_REFER        | \
    LANDLOCK_ACCESS_FS_TRUNCATE     )

/* ============================================================
 * Landlock 网络访问权限位（ABI v4+）
 * ============================================================ */
#define LANDLOCK_ACCESS_NET_BIND_TCP        (1ULL << 0)
#define LANDLOCK_ACCESS_NET_CONNECT_TCP     (1ULL << 1)

#define LANDLOCK_ACCESS_NET_ALL ( \
    LANDLOCK_ACCESS_NET_BIND_TCP    | \
    LANDLOCK_ACCESS_NET_CONNECT_TCP )

/* ============================================================
 * Landlock 规则类型
 * ============================================================ */
enum landlock_rule_type {
    LANDLOCK_RULE_PATH_BENEATH = 1,     /* 路径层级规则 */
    LANDLOCK_RULE_NET_PORT     = 2,     /* 网络端口规则 */
};

/* ============================================================
 * Landlock 用户态接口结构体
 * ============================================================ */

/* landlock_create_ruleset() 属性 */
struct landlock_ruleset_attr {
    u64     handled_access_fs;      /* 要限制的文件系统访问类型 */
    u64     handled_access_net;     /* 要限制的网络访问类型 */
};

/* landlock_add_rule() 的路径规则属性 */
struct landlock_path_beneath_attr {
    u64     allowed_access;         /* 允许的访问权限 */
    int     parent_fd;              /* 父目录的 fd（简化：使用路径名索引）*/
};

/* landlock_add_rule() 的网络规则属性 */
struct landlock_net_port_attr {
    u64     allowed_access;         /* 允许的网络权限 */
    u64     port;                   /* 端口号 */
};

/* ============================================================
 * Landlock 内核数据结构
 * ============================================================ */

/* 最大规则数 */
#define LANDLOCK_MAX_RULES      16

/* 最大规则集数 */
#define LANDLOCK_MAX_RULESETS    8

/* 单条规则（路径 → 允许的权限）*/
struct landlock_rule {
    int             used;               /* 是否已分配 */
    enum landlock_rule_type type;       /* 规则类型 */

    /* 路径规则（LANDLOCK_RULE_PATH_BENEATH）*/
    char            path[64];           /* 路径前缀（简化：存储路径字符串）*/
    u64             allowed_access;     /* 允许的访问权限 */

    /* 网络规则（LANDLOCK_RULE_NET_PORT）*/
    u16             port;               /* 端口号 */
    u64             allowed_net_access; /* 允许的网络权限 */
};

/* 规则集 */
struct landlock_ruleset {
    int                     used;           /* 是否已分配 */
    int                     refcount;       /* 引用计数 */
    u64                     handled_access_fs;   /* 限制的文件系统访问类型 */
    u64                     handled_access_net;  /* 限制的网络访问类型 */
    u32                     num_rules;      /* 规则数 */
    struct landlock_rule    rules[LANDLOCK_MAX_RULES]; /* 规则数组 */
    bool                    enforced;       /* 是否已激活（restrict_self 后为 true）*/
};

/* ============================================================
 * Landlock 接口函数
 * ============================================================ */

/* 初始化 Landlock 子系统 */
void landlock_init(void);

/* 系统调用：创建规则集 */
int landlock_create_ruleset(const struct landlock_ruleset_attr *attr,
                            size_t size, u32 flags);

/* 系统调用：向规则集添加规则 */
int landlock_add_rule(int ruleset_fd, enum landlock_rule_type rule_type,
                      const void *rule_attr, u32 flags);

/* 系统调用：激活沙箱（不可逆）*/
int landlock_restrict_self(int ruleset_fd, u32 flags);

/* 规则集管理 */
struct landlock_ruleset *landlock_get_ruleset(int fd);

/* LSM 钩子：检查文件访问 */
int landlock_file_open(const char *pathname, u64 access_request);

/* LSM 钩子：检查网络访问 */
int landlock_socket_connect(u16 port);

/* 获取当前进程的 Landlock 域 */
struct landlock_ruleset *landlock_get_current_domain(void);

/* 路径辅助：添加路径规则 */
int landlock_add_path_rule(int ruleset_fd, const char *path, u64 allowed_access);

/* 网络辅助：添加端口规则 */
int landlock_add_net_rule(int ruleset_fd, u16 port, u64 allowed_access);

#endif /* __LINUX_LANDLOCK_H */
```
