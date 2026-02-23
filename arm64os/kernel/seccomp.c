/* SPDX-License-Identifier: GPL-2.0 */
/*
 * arm64os/kernel/seccomp.c
 *
 * seccomp 系统调用过滤框架
 *
 * 参考：kernel/seccomp.c
 *
 * Phase 12 实现：
 *   - seccomp_init()：初始化过滤器池
 *   - seccomp_set_mode_strict()：STRICT 模式（只允许 read/write/exit）
 *   - seccomp_set_mode_filter()：FILTER 模式（经典 BPF 过滤器）
 *   - __secure_computing()：每次系统调用前执行过滤检查
 *   - Classic BPF 解释器：执行 sock_filter 指令
 *
 * seccomp 工作流程：
 *   用户态: prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, &prog)
 *       │
 *       ▼
 *   系统调用执行前（do_el0_svc）：
 *       │
 *       ▼
 *   __secure_computing(syscall_number)
 *       ├─→ SECCOMP_MODE_STRICT: 只允许 read/write/exit
 *       └─→ SECCOMP_MODE_FILTER:
 *               │ 执行 BPF 程序
 *               ├─→ SECCOMP_RET_ALLOW  → 继续执行
 *               ├─→ SECCOMP_RET_KILL   → 杀死进程
 *               └─→ SECCOMP_RET_ERRNO  → 返回指定错误码
 */

#include <linux/types.h>
#include <linux/seccomp.h>
#include <linux/sched.h>

/* 外部函数 */
void boot_printk(const char *s);
void boot_printk_hex(unsigned long val);
extern struct task_struct *current_task;

/* ============================================================
 * 过滤器静态池
 *
 * 简化实现：使用静态数组，不需要动态内存分配。
 * ============================================================ */
static struct seccomp_filter filter_pool[MAX_SECCOMP_FILTERS];

/* BPF 程序指令缓冲（每个过滤器最多 32 条指令）*/
#define MAX_BPF_INSNS_PER_FILTER 32
static struct sock_filter insn_pool[MAX_SECCOMP_FILTERS][MAX_BPF_INSNS_PER_FILTER];

/*
 * seccomp_init - 初始化 seccomp 子系统
 */
void seccomp_init(void)
{
    int i;
    for (i = 0; i < MAX_SECCOMP_FILTERS; i++) {
        filter_pool[i].refcount = 0;
        filter_pool[i].log = false;
        filter_pool[i].prev = NULL;
        filter_pool[i].prog.len = 0;
        filter_pool[i].prog.insns = NULL;
    }
    boot_printk("[seccomp] seccomp subsystem initialized\n");
}

/*
 * alloc_filter - 从静态池分配一个过滤器
 */
static struct seccomp_filter *alloc_filter(void)
{
    int i;
    for (i = 0; i < MAX_SECCOMP_FILTERS; i++) {
        if (filter_pool[i].refcount == 0) {
            filter_pool[i].refcount = 1;
            filter_pool[i].log = false;
            filter_pool[i].prev = NULL;
            filter_pool[i].prog.len = 0;
            filter_pool[i].prog.insns = insn_pool[i];
            return &filter_pool[i];
        }
    }
    return NULL;
}

/* ============================================================
 * Classic BPF 解释器
 *
 * 执行一组 sock_filter 指令，以 seccomp_data 为输入数据。
 * 返回 BPF 程序的返回值（SECCOMP_RET_*）。
 *
 * 寄存器：
 *   A（累加器）：运算和比较的主操作数
 *   X（索引寄存器）：辅助操作数
 *   M[0..15]：暂存区（16 个 32 位 slot）
 *
 * 参考：net/core/filter.c __bpf_prog_run()（经典 BPF 部分）
 * ============================================================ */
static u32 run_cbpf(const struct sock_filter *insns, u32 len,
                    const struct seccomp_data *sd)
{
    u32 A = 0;                      /* 累加器 */
    u32 X = 0;                      /* 索引寄存器 */
    u32 mem[BPF_MEMWORDS];          /* 暂存区 */
    const u8 *data = (const u8 *)sd;
    u32 pc;
    int i;

    for (i = 0; i < BPF_MEMWORDS; i++)
        mem[i] = 0;

    for (pc = 0; pc < len; pc++) {
        const struct sock_filter *f = &insns[pc];
        u16 code = f->code;

        switch (code) {
        /* ---- 加载指令 ---- */
        case BPF_LD | BPF_W | BPF_ABS:
            /* A = *(u32 *)(data + k) */
            if (f->k + 4 > sizeof(struct seccomp_data))
                return SECCOMP_RET_KILL_PROCESS;
            A = *(u32 *)(data + f->k);
            break;

        case BPF_LD | BPF_H | BPF_ABS:
            /* A = *(u16 *)(data + k) */
            if (f->k + 2 > sizeof(struct seccomp_data))
                return SECCOMP_RET_KILL_PROCESS;
            A = *(u16 *)(data + f->k);
            break;

        case BPF_LD | BPF_B | BPF_ABS:
            /* A = *(u8 *)(data + k) */
            if (f->k + 1 > sizeof(struct seccomp_data))
                return SECCOMP_RET_KILL_PROCESS;
            A = *(u8 *)(data + f->k);
            break;

        case BPF_LD | BPF_W | BPF_IMM:
            /* A = k */
            A = f->k;
            break;

        case BPF_LD | BPF_W | BPF_MEM:
            /* A = M[k] */
            if (f->k < BPF_MEMWORDS)
                A = mem[f->k];
            break;

        case BPF_LDX | BPF_W | BPF_IMM:
            /* X = k */
            X = f->k;
            break;

        case BPF_LDX | BPF_W | BPF_MEM:
            /* X = M[k] */
            if (f->k < BPF_MEMWORDS)
                X = mem[f->k];
            break;

        /* ---- 存储指令 ---- */
        case BPF_ST:
            /* M[k] = A */
            if (f->k < BPF_MEMWORDS)
                mem[f->k] = A;
            break;

        case BPF_STX:
            /* M[k] = X */
            if (f->k < BPF_MEMWORDS)
                mem[f->k] = X;
            break;

        /* ---- ALU 指令 ---- */
        case BPF_ALU | BPF_ADD | BPF_K:
            A += f->k;
            break;

        case BPF_ALU | BPF_SUB | BPF_K:
            A -= f->k;
            break;

        case BPF_ALU | BPF_MUL | BPF_K:
            A *= f->k;
            break;

        case BPF_ALU | BPF_AND | BPF_K:
            A &= f->k;
            break;

        case BPF_ALU | BPF_OR | BPF_K:
            A |= f->k;
            break;

        case BPF_ALU | BPF_LSH | BPF_K:
            A <<= f->k;
            break;

        case BPF_ALU | BPF_RSH | BPF_K:
            A >>= f->k;
            break;

        case BPF_ALU | BPF_ADD | BPF_X:
            A += X;
            break;

        case BPF_ALU | BPF_SUB | BPF_X:
            A -= X;
            break;

        case BPF_ALU | BPF_AND | BPF_X:
            A &= X;
            break;

        case BPF_ALU | BPF_OR | BPF_X:
            A |= X;
            break;

        /* ---- 跳转指令 ---- */
        case BPF_JMP | BPF_JA:
            /* 无条件跳转：pc += k */
            pc += f->k;
            break;

        case BPF_JMP | BPF_JEQ | BPF_K:
            /* if (A == k) pc += jt else pc += jf */
            pc += (A == f->k) ? f->jt : f->jf;
            break;

        case BPF_JMP | BPF_JGT | BPF_K:
            pc += (A > f->k) ? f->jt : f->jf;
            break;

        case BPF_JMP | BPF_JGE | BPF_K:
            pc += (A >= f->k) ? f->jt : f->jf;
            break;

        case BPF_JMP | BPF_JSET | BPF_K:
            pc += (A & f->k) ? f->jt : f->jf;
            break;

        case BPF_JMP | BPF_JEQ | BPF_X:
            pc += (A == X) ? f->jt : f->jf;
            break;

        case BPF_JMP | BPF_JGT | BPF_X:
            pc += (A > X) ? f->jt : f->jf;
            break;

        case BPF_JMP | BPF_JGE | BPF_X:
            pc += (A >= X) ? f->jt : f->jf;
            break;

        case BPF_JMP | BPF_JSET | BPF_X:
            pc += (A & X) ? f->jt : f->jf;
            break;

        /* ---- 返回指令 ---- */
        case BPF_RET | BPF_K:
            /* 返回立即数 k */
            return f->k;

        case BPF_RET | BPF_A:
            /* 返回累加器 A */
            return A;

        /* ---- 杂项 ---- */
        case BPF_MISC | BPF_TAX:
            /* X = A */
            X = A;
            break;

        case BPF_MISC | BPF_TXA:
            /* A = X */
            A = X;
            break;

        default:
            /* 未知指令 → 杀死进程 */
            return SECCOMP_RET_KILL_PROCESS;
        }
    }

    /* 程序未显式返回 → 杀死进程 */
    return SECCOMP_RET_KILL_PROCESS;
}

/* ============================================================
 * seccomp_set_mode_strict - 启用 STRICT 模式
 *
 * 只允许 read(63), write(64), exit(93), exit_group(94)。
 * 其他系统调用将导致进程被杀死。
 *
 * 参考：kernel/seccomp.c seccomp_set_mode_strict()
 * ============================================================ */
int seccomp_set_mode_strict(void)
{
    if (!current_task)
        return -EINVAL;

    if (current_task->seccomp_mode != SECCOMP_MODE_DISABLED)
        return -EINVAL;     /* 不能从 FILTER 切回 STRICT */

    current_task->seccomp_mode = SECCOMP_MODE_STRICT;
    return 0;
}

/* ============================================================
 * seccomp_set_mode_filter - 安装 BPF 过滤器
 *
 * 将用户提供的 sock_fprog 编译并安装为 seccomp 过滤器。
 * 过滤器可叠加：新过滤器链接到 prev。
 *
 * 参考：kernel/seccomp.c seccomp_set_mode_filter()
 * ============================================================ */
int seccomp_set_mode_filter(struct sock_fprog *fprog)
{
    struct seccomp_filter *filter;
    u32 i;

    if (!current_task || !fprog || !fprog->filter)
        return -EINVAL;

    if (fprog->len == 0 || fprog->len > MAX_BPF_INSNS_PER_FILTER)
        return -EINVAL;

    /* 分配过滤器 */
    filter = alloc_filter();
    if (!filter) {
        boot_printk("[seccomp] WARN: filter pool exhausted\n");
        return -EINVAL;
    }

    /* 复制指令 */
    for (i = 0; i < fprog->len; i++)
        filter->prog.insns[i] = fprog->filter[i];
    filter->prog.len = fprog->len;

    /* 链接到前一个过滤器 */
    filter->prev = (struct seccomp_filter *)current_task->seccomp_filter;

    /* 安装 */
    current_task->seccomp_filter = filter;
    current_task->seccomp_mode = SECCOMP_MODE_FILTER;

    return 0;
}

/* ============================================================
 * __secure_computing - 系统调用过滤入口
 *
 * 在每次系统调用分发前由 do_el0_svc 调用。
 *
 * 返回值：
 *   0             → 允许系统调用继续
 *   负数          → 拒绝（返回值作为 errno 的负值）
 *   SECCOMP_RET_KILL → 进程应被杀死
 *
 * 参考：kernel/seccomp.c __secure_computing()
 * ============================================================ */
int __secure_computing(const struct seccomp_data *sd)
{
    int mode;
    struct seccomp_filter *filter;
    u32 action;

    if (!current_task)
        return 0;

    mode = current_task->seccomp_mode;

    if (mode == SECCOMP_MODE_DISABLED)
        return 0;

    if (mode == SECCOMP_MODE_STRICT) {
        /* STRICT 模式：仅允许白名单系统调用 */
        switch (sd->nr) {
        case __NR_read:
        case __NR_write:
        case __NR_exit:
        case __NR_exit_group:
            return 0;
        default:
            return -EACCES;
        }
    }

    if (mode == SECCOMP_MODE_FILTER) {
        /* FILTER 模式：执行 BPF 过滤器链 */
        filter = (struct seccomp_filter *)current_task->seccomp_filter;
        action = SECCOMP_RET_ALLOW;

        /* 遍历过滤器链（从最新到最旧）*/
        while (filter) {
            u32 cur_ret = run_cbpf(filter->prog.insns,
                                   filter->prog.len, sd);

            /* 取最严格的返回值（数值越小越严格）*/
            if ((cur_ret & SECCOMP_RET_ACTION_FULL) <
                (action & SECCOMP_RET_ACTION_FULL))
                action = cur_ret;

            filter = filter->prev;
        }

        switch (action & SECCOMP_RET_ACTION_FULL) {
        case SECCOMP_RET_ALLOW:
            return 0;

        case SECCOMP_RET_ERRNO:
            /* 返回用户指定的错误码 */
            return -(int)(action & SECCOMP_RET_DATA);

        case SECCOMP_RET_KILL_PROCESS:
        case SECCOMP_RET_KILL_THREAD:
            /* 杀死进程 */
            return -(int)EACCES;

        case SECCOMP_RET_TRAP:
            return -(int)EACCES;

        default:
            return -(int)EACCES;
        }
    }

    return 0;
}
