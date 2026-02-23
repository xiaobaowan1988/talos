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
