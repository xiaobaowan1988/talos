# Phase 12：seccomp + eBPF JIT + Landlock LSM

## 知识来源总览

- **BPF 指令集规范**：约 30%（经典 BPF 指令、eBPF 64 位 RISC 架构、11 寄存器模型）
- **ARM64 JIT 编译**：约 25%（eBPF→AArch64 寄存器映射、指令编码翻译）
- **seccomp 机制**：约 20%（过滤器链、SECCOMP_RET_* 返回值语义）
- **Landlock LSM**：约 15%（路径层级访问控制、inode 红黑树、不可逆沙箱）
- **前序依赖**：约 10%

## seccomp：系统调用过滤

seccomp（secure computing）在系统调用执行前拦截，决定是否允许执行。它是容器安全的第一道防线——通过缩小攻击面，使得即使内核存在漏洞，容器也无法触及危险的系统调用。

```
用户态: prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, &prog)
    │
    ▼
系统调用执行前：
    │
    ▼
secure_computing(syscall_number)
    │
    └─→ SECCOMP_MODE_FILTER:
            │ 执行 BPF 程序
            ├─→ SECCOMP_RET_ALLOW  → 继续执行
            ├─→ SECCOMP_RET_KILL   → 立即杀死进程（SIGSYS）
            ├─→ SECCOMP_RET_TRAP   → 发送 SIGSYS（可被捕获）
            ├─→ SECCOMP_RET_ERRNO  → 返回指定错误码
            └─→ SECCOMP_RET_TRACE  → 通知 ptrace 追踪器
```

### 过滤器链：不可逆的安全叠加

```c
struct seccomp_filter {
    refcount_t          usage;
    bool                log;
    struct seccomp_filter *prev;    /* 过滤器链（可叠加）*/
    struct bpf_prog     *prog;      /* BPF 程序 */
};
```

**`prev` 指针形成单向链表**：每次 `prctl(PR_SET_SECCOMP)` 都在链表头部插入新的过滤器。`prev` 指向之前安装的过滤器。这个链表只能增长，不能删除——安全策略只能变得更严格，不能放松。

**为什么不可逆？** 如果允许移除过滤器，攻击者利用代码执行漏洞后的第一件事就是卸载 seccomp 限制。不可逆性确保了即使进程被攻破，安全策略仍然生效。

```c
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

**取最严格的返回值**：`SECCOMP_RET_KILL(0) < SECCOMP_RET_TRAP < SECCOMP_RET_ERRNO < SECCOMP_RET_ALLOW(0x7fff0000)`。数值越小越严格。遍历所有过滤器后取最小值——任何一个过滤器说 KILL，最终结果就是 KILL。

**`seccomp_data` 结构**：传入 BPF 程序的上下文，包含 `nr`（系统调用号）、`arch`（架构标识）、`instruction_pointer`、`args[6]`（系统调用参数）。BPF 程序可以检查系统调用号和参数来做精细控制。

### seccomp BPF 程序示例

```c
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
```

**`BPF_JUMP` 的两个偏移**：`BPF_JUMP(op, k, jt, jf)` 中 `jt` 是条件为真时跳过的指令数，`jf` 是为假时跳过的指令数。`BPF_JUMP(BPF_JEQ, __NR_read, 0, 1)` 意思是：如果等于 `__NR_read`，跳 0 条（继续下一条 ALLOW），否则跳 1 条（跳过 ALLOW 到下一个检查）。

**经典 BPF vs eBPF**：seccomp 使用经典 BPF（cBPF），只有 2 个寄存器（A 和 X），32 位操作。内核在加载时自动将 cBPF 转换为 eBPF 执行。

## eBPF 架构与 JIT 编译

```
eBPF 完整执行链路：

用户态程序 (C代码)
    │ clang -target bpf
    ▼
eBPF 字节码 (.o)
    │ bpf(BPF_PROG_LOAD, ...)
    ▼
内核 verifier
    │ 静态分析：安全性、终止性、内存访问
    ▼
JIT 编译器
    │ eBPF指令 → AArch64机器码
    ▼
执行（直接调用机器码，无解释器开销）
```

### eBPF 指令集

```c
/* eBPF 有 11 个 64 位寄存器：r0-r10 */
/* r0  = 返回值 */
/* r1-r5 = 函数调用参数 */
/* r6-r9 = 被调用者保存寄存器 */
/* r10 = 只读栈帧指针 */

struct bpf_insn {
    __u8    code;       /* 操作码 */
    __u8    dst_reg:4;  /* 目标寄存器 */
    __u8    src_reg:4;  /* 源寄存器 */
    __s16   off;        /* 偏移量 */
    __s32   imm;        /* 立即数 */
};
```

**8 字节固定长度指令**：每条 eBPF 指令恰好 8 字节（1+1+2+4）。固定长度简化了解码和 JIT 编译——不需要变长指令的复杂解析逻辑。

**11 个寄存器的设计**：借鉴了现代 RISC 架构的设计。r1-r5 传参（对应 AArch64 的 x0-x4），r6-r9 是 callee-saved（对应 AArch64 的 x19-x22），r10 是只读帧指针（对应 x25）。r0 是返回值（对应 x7——之所以不用 x0，是因为 x0 已经映射给 r1 做第一参数）。

**`dst_reg:4` 位域**：4 位可以编码 0-15，但 eBPF 只用 r0-r10（11 个），留有扩展空间。`src_reg` 和 `dst_reg` 合占 1 字节，高 4 位是 src，低 4 位是 dst。

### Verifier：静态安全验证

eBPF verifier 是整个 eBPF 安全模型的基石。它在加载时（而非运行时）对字节码做**抽象解释**（abstract interpretation）：

1. **DAG 检测**：确保控制流图是 DAG（无环），保证程序必定终止
2. **寄存器状态追踪**：每个分支点记录所有寄存器的类型（指针/标量/未初始化）
3. **内存访问边界检查**：确保所有 load/store 都在合法范围内
4. **指针算术限制**：不允许任意指针运算，只允许 `base + offset` 形式
5. **helper 函数参数类型检查**：确保传给内核 helper 的参数类型正确

### ARM64 JIT 编译器

```c
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
```

**映射的精妙设计**：

- **r1→x0, r2→x1, ..., r5→x4**：eBPF 的参数寄存器直接映射到 AAPCS64 的参数寄存器。调用内核 helper 函数时无需移动寄存器——eBPF r1 里的值已经在 x0 中，直接 `blr` 即可。
- **r6→x19, r7→x20, r8→x21, r9→x22**：eBPF 的 callee-saved 映射到 AArch64 的 callee-saved（x19-x28）。调用 helper 后这些值自动保持不变。
- **r0→x7**：返回值用 x7 而非 x0，因为 x0 给了 r1。`BPF_EXIT` 翻译时需要 `mov x0, x7` 把返回值移到正确位置。

```c
static int build_insn(const struct bpf_insn *insn, struct jit_ctx *ctx) {
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

**逐条翻译**：每条 eBPF 指令翻译为 1-3 条 AArch64 指令。`BPF_ADD` 直接变 `add`，`BPF_LDX` 直接变 `ldr`，几乎是一对一映射——这正是 eBPF RISC 指令集设计的初衷。

**`A64_ADD_I(1, dst, dst, imm)` 中的 `1`**：表示 64 位操作（sf=1）。如果是 `BPF_ALU`（32 位）则传 0，生成 `add w_dst, w_dst, #imm`。

**两遍扫描**：JIT 编译器先做一遍扫描计算每条指令的偏移（因为跳转目标需要知道目标指令的地址），第二遍才生成实际机器码。`ctx->offset[i]` 记录第 i 条 eBPF 指令对应的机器码偏移。

## Landlock LSM：路径级访问控制

Landlock 是 Linux 5.13 引入的 LSM（Linux Security Module），允许**无特权进程**限制自身的文件系统和网络访问。与 seccomp 不同，Landlock 工作在路径层级而非系统调用层级。

### 用户态接口

```c
void landlock_sandbox(void) {
    /* 1. 创建规则集 */
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

    /* 3. 激活沙箱（不可逆！）*/
    prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0);
    landlock_restrict_self(ruleset_fd, 0);
}
```

**`handled_access_fs` 的含义**：不是"允许这些操作"，而是"这些操作由 Landlock 管控"。未列入的操作不受 Landlock 限制。这个设计支持向前兼容——新版内核添加新的访问类型时，旧程序不会因为没有声明新类型而被拒绝。

**`O_PATH` 打开目录**：不需要读权限，只获取一个路径引用。这允许在沙箱激活前获取目录的 fd，即使沙箱激活后不再能 open 该路径。

**`PR_SET_NO_NEW_PRIVS` 是前置条件**：确保进程不能通过 exec setuid 程序来逃逸沙箱。这与 seccomp 的要求一致——Phase 5 中的 execve 如果遇到 setuid 二进制文件，no_new_privs 标志会阻止特权提升。

### 内核实现

```c
struct landlock_ruleset {
    struct rb_root      root_inode;   /* 按 inode 排序的规则树 */
    refcount_t          usage;
    u64                 handled_access_fs;
    u32                 num_layers;   /* 嵌套深度 */
};

struct landlock_rule {
    struct rb_node      node;
    struct landlock_object *object;   /* inode 对象 */
    u32                 num_layers;
    struct landlock_layer layers[];   /* 每层的允许权限 */
};
```

**红黑树索引**：规则按 inode 地址组织在红黑树中，查找时间 O(log n)。为什么用 inode 而不是路径字符串？因为同一个文件可以有多个路径名（硬链接），但只有一个 inode——用 inode 做 key 避免了绕过。

**`layers[]` 数组**：支持规则叠加。外层沙箱设置 layer 0，内层沙箱设置 layer 1。检查时所有层的权限取交集——最终允许的权限是所有层都允许的权限。

```c
static int check_access_path(const struct landlock_ruleset *domain,
                             const struct path *path, u64 access_request) {
    struct path walker = *path;

    /* 从文件向上遍历到根目录 */
    while (true) {
        const struct landlock_rule *rule =
            find_rule(domain, walker.dentry->d_inode);

        if (rule) {
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

**向上遍历路径**：检查 `/etc/hostname` 时，先查 `hostname` 的 inode，没有规则则查 `etc` 的 inode，再查 `/` 的 inode。如果规则允许 `/etc` 的读取，那么 `/etc` 下所有文件都可以读取——这就是 `LANDLOCK_RULE_PATH_BENEATH` 的语义。

## 三者协同：容器安全纵深防御

```
容器安全层次（由外到内）：

1. Landlock（路径级沙箱）
   └── 限制文件系统和网络访问的路径范围

2. seccomp（系统调用白名单）
   └── 过滤危险系统调用（mount, ptrace, kexec）

3. eBPF（可编程策略引擎）
   └── 网络过滤、参数检查、安全监控

4. cgroup（资源限制，Phase 10）
   └── 防止资源耗尽型攻击

5. namespace（视图隔离，Phase 10）
   └── 防止信息泄露和横向移动
```

**纵深防御的核心思想**：每一层独立工作，任何一层被绕过，其他层仍然有效。seccomp 阻止调用 `mount`，即使 seccomp 被绕过，namespace 隔离确保 mount 只影响容器内部；即使 namespace 被逃逸，Landlock 限制了可访问的文件路径。

**与前序 Phase 的关系**：
- Phase 5（系统调用）→ seccomp 在系统调用入口处拦截
- Phase 7（VFS）→ Landlock 在 VFS 的 LSM hook 点检查路径权限
- Phase 10（namespace/cgroup）→ 提供隔离和资源限制的基础层
- Phase 11（netfilter）→ eBPF 可以附加到网络 hook 点做流量过滤

至此，12 个阶段的全部内容完成。从裸机启动的第一条汇编指令，到能够运行安全容器的完整内核，每一行代码都可以追溯到它的知识来源——ARM 架构手册、RFC 协议规范、Linux 内核设计模式、或 POSIX 标准的语义约束。
