/* SPDX-License-Identifier: GPL-2.0 */
/*
 * arm64os/kernel/bpf/syscall.c
 *
 * bpf() 系统调用实现
 *
 * 参考：kernel/bpf/syscall.c
 *
 * Phase 12 实现：
 *   - BPF_PROG_LOAD：加载 eBPF 程序（验证 + 可选 JIT）
 *   - BPF_PROG_RUN：测试运行 eBPF 程序
 */

#include <linux/types.h>
#include <linux/bpf.h>

/* 外部函数 */
void boot_printk(const char *s);
void boot_printk_hex(unsigned long val);

/* kernel/bpf/core.c */
int bpf_prog_alloc(void);

/* ============================================================
 * sys_bpf - bpf() 系统调用处理
 *
 * 参数：
 *   cmd    — 命令（BPF_PROG_LOAD, BPF_PROG_RUN）
 *   attr   — 属性结构体
 *   size   — attr 的大小
 *
 * 返回值：
 *   BPF_PROG_LOAD → 程序 fd（>= 0）或负数错误码
 *   BPF_PROG_RUN  → 0（retval 存入 attr->retval）或负数错误码
 *
 * 参考：kernel/bpf/syscall.c __sys_bpf()
 * ============================================================ */
int sys_bpf(int cmd, struct bpf_attr *attr, u32 size)
{
    if (!attr)
        return -1;

    switch (cmd) {
    case BPF_PROG_LOAD: {
        int fd;
        struct ebpf_prog *prog;
        u32 i;

        /* 检查指令数 */
        if (attr->insn_cnt == 0 || attr->insn_cnt > BPF_MAXINSNS) {
            boot_printk("[bpf] PROG_LOAD: invalid insn_cnt\n");
            return -1;
        }

        if (!attr->insns) {
            boot_printk("[bpf] PROG_LOAD: insns is NULL\n");
            return -1;
        }

        /* 分配程序槽 */
        fd = bpf_prog_alloc();
        if (fd < 0) {
            boot_printk("[bpf] PROG_LOAD: prog pool exhausted\n");
            return -1;
        }

        prog = bpf_prog_get(fd);
        if (!prog)
            return -1;

        /* 复制指令 */
        prog->type = (enum bpf_prog_type)attr->prog_type;
        prog->len = attr->insn_cnt;
        for (i = 0; i < attr->insn_cnt; i++)
            prog->insns[i] = attr->insns[i];

        /* 验证 */
        if (bpf_verify(prog) != 0) {
            boot_printk("[bpf] PROG_LOAD: verification failed\n");
            prog->used = 0;
            return -1;
        }

        /* JIT 编译（如果启用）*/
        if (bpf_jit_enable) {
            if (bpf_jit_compile(prog) == 0) {
                /* JIT 成功 — prog->jit_done = true 已设置 */
            }
            /* JIT 失败不是错误，回退解释器 */
        }

        boot_printk("[bpf] PROG_LOAD: fd=");
        boot_printk_hex((unsigned long)fd);
        boot_printk(", len=");
        boot_printk_hex((unsigned long)prog->len);
        if (prog->jit_done)
            boot_printk(" (JIT)\n");
        else
            boot_printk(" (interp)\n");

        return fd;
    }

    case BPF_PROG_RUN: {
        struct ebpf_prog *prog;
        u64 retval;

        prog = bpf_prog_get((int)attr->prog_fd);
        if (!prog) {
            boot_printk("[bpf] PROG_RUN: invalid prog_fd\n");
            return -1;
        }

        /* 执行程序 */
        retval = bpf_prog_run(prog, attr->data_in);
        attr->retval = (u32)retval;

        return 0;
    }

    default:
        boot_printk("[bpf] unknown cmd: ");
        boot_printk_hex((unsigned long)cmd);
        boot_printk("\n");
        return -1;
    }
}
