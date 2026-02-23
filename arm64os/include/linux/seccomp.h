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
