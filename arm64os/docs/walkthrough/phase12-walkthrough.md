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
