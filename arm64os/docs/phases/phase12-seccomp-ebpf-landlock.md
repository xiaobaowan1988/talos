# Phase 12：seccomp + eBPF JIT + Landlock LSM

## 参考内核文件

```
kernel/seccomp.c                    # seccomp系统调用过滤
include/uapi/linux/seccomp.h        # seccomp用户态接口
include/uapi/linux/filter.h         # BPF指令集（经典BPF）
kernel/bpf/core.c                   # eBPF解释器与JIT框架
kernel/bpf/verifier.c               # eBPF字节码验证器
kernel/bpf/syscall.c                # bpf()系统调用
arch/arm64/net/bpf_jit_comp.c      # ARM64 eBPF JIT编译器
include/linux/bpf.h                 # eBPF数据结构
security/landlock/fs.c              # Landlock文件系统访问控制
security/landlock/net.c             # Landlock网络访问控制
security/landlock/ruleset.c         # Landlock规则集管理
include/uapi/linux/landlock.h       # Landlock用户态接口
```

---

## 12.1 seccomp（系统调用过滤）

seccomp 限制进程可以调用的系统调用，是容器安全的重要防线：

```
seccomp 工作流程：

用户态: prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, &prog)
    │
    ▼
系统调用执行前（arch/arm64/kernel/syscall.c）：
    │
    ▼
secure_computing(syscall_number)
    │
    ├─→ SECCOMP_MODE_STRICT: 只允许 read/write/exit/sigreturn
    │
    └─→ SECCOMP_MODE_FILTER:
            │ 执行 BPF 程序
            ├─→ SECCOMP_RET_ALLOW  → 继续执行
            ├─→ SECCOMP_RET_KILL   → 立即杀死进程（SIGSYS）
            ├─→ SECCOMP_RET_TRAP   → 发送 SIGSYS（可被捕获）
            ├─→ SECCOMP_RET_ERRNO  → 返回指定错误码
            └─→ SECCOMP_RET_TRACE  → 通知 ptrace 追踪器
```

```c
/* 参考 kernel/seccomp.c */

struct seccomp_filter {
    refcount_t          usage;
    bool                log;
    struct seccomp_filter *prev;    /* 过滤器链（可叠加）*/
    struct bpf_prog     *prog;      /* BPF 程序 */
};

/* 对每个系统调用执行过滤 */
int __secure_computing(const struct seccomp_data *sd) {
    struct seccomp_filter *filter = current->seccomp.filter;
    u32 action = SECCOMP_RET_ALLOW;

    /* 遍历过滤器链（从最新到最旧）*/
    for (; filter; filter = filter->prev) {
        u32 cur_ret = bpf_prog_run(filter->prog, sd);
        /* 取最严格的返回值 */
        if ((cur_ret & SECCOMP_RET_ACTION_FULL) < action)
            action = cur_ret & SECCOMP_RET_ACTION_FULL;
    }

    switch (action) {
    case SECCOMP_RET_ALLOW:
        return 0;
    case SECCOMP_RET_KILL_PROCESS:
        do_group_exit(SIGSYS);
        break;
    case SECCOMP_RET_ERRNO:
        syscall_set_return_value(current, current_pt_regs(),
                                 -1, -(action & SECCOMP_RET_DATA));
        return -1;
    }
    return -1;
}
```

### seccomp BPF程序示例（用户态）

```c
/* 参考 include/uapi/linux/filter.h */
/* 只允许 read/write/exit/exit_group，其他系统调用返回 EPERM */

struct sock_filter filter[] = {
    /* 加载系统调用号到累加器 */
    BPF_STMT(BPF_LD | BPF_W | BPF_ABS,
             offsetof(struct seccomp_data, nr)),

    /* 允许 read(0) */
    BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, __NR_read,  0, 1),
    BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),

    /* 允许 write(1) */
    BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, __NR_write, 0, 1),
    BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),

    /* 允许 exit(60) */
    BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, __NR_exit,  0, 1),
    BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),

    /* 其他：返回 EPERM */
    BPF_STMT(BPF_RET | BPF_K,
             SECCOMP_RET_ERRNO | (EPERM & SECCOMP_RET_DATA)),
};

struct sock_fprog prog = {
    .len    = ARRAY_SIZE(filter),
    .filter = filter,
};
prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, &prog);
```

---

## 12.2 eBPF架构与JIT编译（参考 kernel/bpf/）

```
eBPF 完整执行链路：

用户态程序 (C代码)
    │ clang -target bpf
    ▼
eBPF 字节码 (.o)
    │ bpf(BPF_PROG_LOAD, ...)
    ▼
内核 verifier（kernel/bpf/verifier.c）
    │ 静态分析：安全性、终止性、内存访问
    ▼
JIT 编译器（arch/arm64/net/bpf_jit_comp.c）
    │ eBPF指令 → AArch64机器码
    ▼
执行（直接调用机器码，无解释器开销）
    │
    ▼
返回结果 → 内核根据返回值决策
```

### eBPF指令集（64位RISC）

```c
/* 参考 include/linux/bpf.h */
/* eBPF 有 11 个64位寄存器：r0-r10 */
/* r0  = 返回值 */
/* r1-r5 = 函数调用参数（调用 helper 函数时）*/
/* r6-r9 = 被调用者保存寄存器 */
/* r10 = 只读栈帧指针 */

struct bpf_insn {
    __u8    code;       /* 操作码 */
    __u8    dst_reg:4;  /* 目标寄存器 */
    __u8    src_reg:4;  /* 源寄存器 */
    __s16   off;        /* 偏移量（内存访问或跳转目标）*/
    __s32   imm;        /* 立即数 */
};

/* eBPF 操作码示例 */
#define BPF_MOV64_IMM(DST, IMM)  \
    ((struct bpf_insn){ .code  = BPF_ALU64 | BPF_MOV | BPF_K, \
                        .dst_reg = DST, .src_reg = 0, \
                        .off = 0, .imm = IMM })

#define BPF_EXIT_INSN()  \
    ((struct bpf_insn){ .code = BPF_JMP | BPF_EXIT })
```

### ARM64 JIT编译器核心（参考 arch/arm64/net/bpf_jit_comp.c）

```c
/* 参考 arch/arm64/net/bpf_jit_comp.c */
/*
 * eBPF 寄存器到 AArch64 寄存器映射：
 * eBPF r0  → x7   (返回值)
 * eBPF r1  → x0   (第1参数)
 * eBPF r2  → x1
 * eBPF r3  → x2
 * eBPF r4  → x3
 * eBPF r5  → x4
 * eBPF r6  → x19  (被调用者保存)
 * eBPF r7  → x20
 * eBPF r8  → x21
 * eBPF r9  → x22
 * eBPF r10 → x25  (栈帧指针)
 */

/* 编译单条 eBPF 指令为 AArch64 机器码 */
static int build_insn(const struct bpf_insn *insn, struct jit_ctx *ctx) {
    const u8 code = insn->code;
    const u8 dst  = bpf2a64[insn->dst_reg];
    const u8 src  = bpf2a64[insn->src_reg];

    switch (code) {
    /* eBPF: dst += imm → AArch64: add dst, dst, #imm */
    case BPF_ALU64 | BPF_ADD | BPF_K:
        emit(A64_ADD_I(1, dst, dst, insn->imm), ctx);
        break;

    /* eBPF: dst = *(u64 *)(src + off) → AArch64: ldr dst, [src, #off] */
    case BPF_LDX | BPF_MEM | BPF_DW:
        emit(A64_LDR64(dst, src, insn->off), ctx);
        break;

    /* eBPF: if dst == imm goto +off → AArch64: cmp + b.eq */
    case BPF_JMP | BPF_JEQ | BPF_K:
        emit(A64_CMP_I(1, dst, insn->imm), ctx);
        emit(A64_B_COND(A64_COND_EQ, off), ctx);
        break;

    /* eBPF: exit → AArch64: mov x0, x7; ret */
    case BPF_JMP | BPF_EXIT:
        emit(A64_MOV(1, A64_R(0), bpf2a64[BPF_REG_0]), ctx);
        emit(A64_RET(A64_LR), ctx);
        break;
    }
    return 0;
}
```

---

## 12.3 Landlock LSM（参考 security/landlock/）

Landlock 是一种**不可权限提升的**沙箱机制：允许无特权进程限制自身的文件系统和网络访问。

```
Landlock 设计原则：
1. 无需 root 权限（普通进程可自我沙箱化）
2. 不可绕过（子进程继承且无法放松规则）
3. 规则叠加（只能更严，不能更松）
4. 基于路径层级访问控制（非 DAC/MAC）
```

### Landlock用户态接口

```c
/* 参考 include/uapi/linux/landlock.h */

/* 文件系统访问权限位 */
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

/* 网络访问权限位（Landlock ABI v4+）*/
#define LANDLOCK_ACCESS_NET_BIND_TCP        (1ULL << 0)
#define LANDLOCK_ACCESS_NET_CONNECT_TCP     (1ULL << 1)

/* 使用示例：只允许读取 /etc，写入 /tmp */
void landlock_sandbox(void) {
    /* 1. 创建规则集：声明要限制的访问类型 */
    struct landlock_ruleset_attr rs_attr = {
        .handled_access_fs =
            LANDLOCK_ACCESS_FS_EXECUTE   |
            LANDLOCK_ACCESS_FS_WRITE_FILE |
            LANDLOCK_ACCESS_FS_READ_FILE  |
            LANDLOCK_ACCESS_FS_READ_DIR,
    };
    int ruleset_fd = landlock_create_ruleset(&rs_attr, sizeof(rs_attr), 0);

    /* 2. 添加规则：允许读取 /etc */
    int etc_fd = open("/etc", O_PATH | O_CLOEXEC);
    struct landlock_path_beneath_attr path_attr = {
        .allowed_access = LANDLOCK_ACCESS_FS_READ_FILE |
                          LANDLOCK_ACCESS_FS_READ_DIR,
        .parent_fd      = etc_fd,
    };
    landlock_add_rule(ruleset_fd, LANDLOCK_RULE_PATH_BENEATH,
                      &path_attr, 0);
    close(etc_fd);

    /* 3. 添加规则：允许读写 /tmp */
    int tmp_fd = open("/tmp", O_PATH | O_CLOEXEC);
    path_attr.allowed_access = LANDLOCK_ACCESS_FS_READ_FILE  |
                               LANDLOCK_ACCESS_FS_WRITE_FILE |
                               LANDLOCK_ACCESS_FS_READ_DIR   |
                               LANDLOCK_ACCESS_FS_MAKE_REG;
    path_attr.parent_fd = tmp_fd;
    landlock_add_rule(ruleset_fd, LANDLOCK_RULE_PATH_BENEATH,
                      &path_attr, 0);
    close(tmp_fd);

    /* 4. 激活沙箱（不可逆！）*/
    prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0);  /* 必须先设置 */
    landlock_restrict_self(ruleset_fd, 0);
    close(ruleset_fd);

    /* 此后：访问 /etc → 允许；访问 /home → EACCES */
}
```

### Landlock内核实现

```c
/* 参考 security/landlock/fs.c */

/* Landlock 规则集 */
struct landlock_ruleset {
    struct rb_root      root_inode;   /* 按inode排序的规则树 */
    refcount_t          usage;
    u64                 handled_access_fs;
    u64                 handled_access_net;
    u32                 num_layers;   /* 嵌套深度 */
    u32                 num_rules;
};

/* 每个规则：路径 → 允许的权限 */
struct landlock_rule {
    struct rb_node      node;
    struct landlock_object *object;   /* inode对象 */
    u32                 num_layers;
    struct landlock_layer layers[];   /* 每层的允许权限 */
};

/* LSM hook：检查文件访问 */
static int hook_file_open(struct file *const file) {
    struct landlock_ruleset *domain =
        landlock_get_current_domain();

    if (!domain) return 0;  /* 未沙箱化进程直接放行 */

    /* 检查文件的每个路径分量是否在规则集中 */
    return check_access_path(domain, &file->f_path,
                             LANDLOCK_ACCESS_FS_READ_FILE);
}

static int check_access_path(const struct landlock_ruleset *domain,
                             const struct path *path, u64 access_request) {
    struct path walker = *path;

    /* 从文件向上遍历到根目录 */
    while (true) {
        /* 在规则树中查找当前路径分量的 inode */
        const struct landlock_rule *rule =
            find_rule(domain, walker.dentry->d_inode);

        if (rule) {
            /* 检查所请求的权限是否被该规则允许 */
            if (landlock_union_access(rule, domain->num_layers) &
                access_request)
                return 0;  /* 允许 */
        }

        if (is_root(walker.dentry)) break;
        walker.dentry = walker.dentry->d_parent;
    }

    return -EACCES;  /* 拒绝 */
}
```

## 12.4 三者协同：容器安全防御纵深

```
容器安全层次（由外到内）：

1. Landlock（用户自愿沙箱）
   ├── 进程主动限制自身文件/网络访问
   └── 即使容器逃逸，仍受路径权限约束

2. seccomp（系统调用白名单）
   ├── 过滤危险系统调用（如 mount, ptrace, kexec）
   └── 缩小攻击面（容器不需要的syscall一律拒绝）

3. eBPF（可编程策略）
   ├── 网络流量过滤（XDP/tc）
   ├── 运行时系统调用参数检查
   └── 性能无损的安全监控（tracepoint/kprobe）

4. cgroup（资源限制，Phase 10）
   └── 防止资源耗尽（DoS）

5. namespace（隔离，Phase 10）
   └── 防止信息泄露和横向移动
```

## 12.5 验证方法

```c
void test_seccomp(void) {
    /* 安装只允许 write/exit 的 seccomp 过滤器 */
    install_seccomp_filter();

    /* 这应该成功 */
    write(1, "seccomp test\n", 13);

    /* 这应该返回 EPERM */
    int ret = open("/etc/passwd", O_RDONLY);
    printk("open after seccomp: %d (expect -1)\n", ret);
}

void test_ebpf_jit(void) {
    /* 加载一个简单的 eBPF 程序：返回系统调用号 */
    struct bpf_insn prog[] = {
        BPF_LDX_MEM(BPF_W, BPF_REG_0, BPF_REG_1,
                    offsetof(struct seccomp_data, nr)),
        BPF_EXIT_INSN(),
    };
    int prog_fd = bpf(BPF_PROG_LOAD, &attr, sizeof(attr));
    printk("eBPF JIT load: fd=%d\n", prog_fd);
}

void test_landlock(void) {
    landlock_sandbox();  /* 只允许读/etc，写/tmp */

    /* 应该成功 */
    int fd = open("/etc/hostname", O_RDONLY);
    printk("read /etc/hostname: fd=%d (expect >=0)\n", fd);

    /* 应该失败 */
    fd = open("/etc/shadow", O_WRONLY);
    printk("write /etc/shadow: fd=%d (expect -1)\n", fd);
}
```

## 12.6 本阶段产出文件

```
arm64os/
├── kernel/
│   ├── seccomp.c               ← seccomp过滤框架（核心）
│   └── bpf/
│       ├── core.c              ← eBPF解释器与JIT调度
│       ├── verifier.c          ← 字节码验证器（核心）
│       └── syscall.c           ← bpf()系统调用
├── arch/arm64/net/
│   └── bpf_jit_comp.c         ← ARM64 JIT编译器（核心）
└── security/
    └── landlock/
        ├── ruleset.c           ← 规则集管理（核心）
        ├── fs.c                ← 文件系统访问控制
        └── net.c               ← 网络访问控制
```

---

## 学习路线图总结

完成 Phase 12 后，已实现一个完整的最小化类Linux内核，涵盖：

```
Phase 1-2:  启动 + 内存管理基础（ARMv8硬件接口）
Phase 3-4:  中断 + 调度（内核并发基础）
Phase 5-6:  用户态接口 + 设备驱动（系统完整性）
Phase 7-8:  VFS + 具体文件系统（存储抽象层）
Phase 9-10: 容器存储 + 资源隔离（容器化基础）
Phase 11:   网络协议栈（现代系统联网能力）
Phase 12:   安全加固（生产环境安全基线）

最终成果：一个能在 QEMU ARM64 上运行容器的最小内核
```
