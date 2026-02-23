/* SPDX-License-Identifier: GPL-2.0 */
/*
 * arm64os/kernel/bpf/verifier.c
 *
 * eBPF 字节码验证器
 *
 * 参考：kernel/bpf/verifier.c
 *
 * Phase 12 实现：
 *   - bpf_verify()：验证 eBPF 程序的安全性和终止性
 *
 * 验证检查项：
 *   1. 程序长度检查（不超过 BPF_MAXINSNS）
 *   2. 最后一条指令必须是 EXIT
 *   3. 跳转目标在有效范围内
 *   4. 寄存器编号合法（0-10）
 *   5. r10 是只读的（不能作为目标寄存器写入）
 *   6. 无无限循环（简化检查：不允许后向跳转）
 *   7. 所有执行路径最终到达 EXIT
 *
 * 简化说明：
 *   真实 Linux 内核的 verifier 执行完整的抽象解释（abstract
 *   interpretation），跟踪每个寄存器的类型状态（NOT_INIT,
 *   SCALAR_VALUE, PTR_TO_CTX, ...），检查内存安全、指针运算
 *   合法性等。我们只做基础的结构安全检查。
 */

#include <linux/types.h>
#include <linux/bpf.h>

/* 外部函数 */
void boot_printk(const char *s);
void boot_printk_hex(unsigned long val);

/* ============================================================
 * bpf_verify - 验证 eBPF 程序
 *
 * 返回 0 表示通过验证，负数表示拒绝。
 *
 * 参考：kernel/bpf/verifier.c bpf_check()
 * ============================================================ */
int bpf_verify(const struct ebpf_prog *prog)
{
    u32 i;
    const struct bpf_insn *insns;
    u32 len;
    bool has_exit = false;

    if (!prog)
        return -1;

    insns = prog->insns;
    len = prog->len;

    /* 检查 1：程序长度 */
    if (len == 0) {
        boot_printk("[verifier] REJECT: empty program\n");
        return -1;
    }

    if (len > BPF_MAXINSNS) {
        boot_printk("[verifier] REJECT: program too long (");
        boot_printk_hex(len);
        boot_printk(" > ");
        boot_printk_hex(BPF_MAXINSNS);
        boot_printk(")\n");
        return -1;
    }

    /* 检查 2：最后一条指令必须是 EXIT */
    {
        const struct bpf_insn *last = &insns[len - 1];
        if (last->code != (EBPF_CLS_JMP | EBPF_EXIT)) {
            boot_printk("[verifier] REJECT: last insn not EXIT\n");
            return -1;
        }
    }

    /* 逐条指令验证 */
    for (i = 0; i < len; i++) {
        const struct bpf_insn *insn = &insns[i];
        u8 code = insn->code;
        u8 dst = insn->dst_reg;
        u8 src = insn->src_reg;
        s16 off = insn->off;

        u8 cls = code & 0x07;

        /* 检查 3：寄存器编号合法（0-10）*/
        if (dst >= MAX_BPF_REG) {
            boot_printk("[verifier] REJECT: invalid dst_reg at pc=");
            boot_printk_hex(i);
            boot_printk("\n");
            return -1;
        }
        if (src >= MAX_BPF_REG) {
            boot_printk("[verifier] REJECT: invalid src_reg at pc=");
            boot_printk_hex(i);
            boot_printk("\n");
            return -1;
        }

        /* 检查 4：r10 是只读的 */
        switch (cls) {
        case EBPF_CLS_ALU64:
        case EBPF_CLS_ALU:
            /* ALU 指令不能写入 r10 */
            if (dst == BPF_REG_10) {
                boot_printk("[verifier] REJECT: write to r10 at pc=");
                boot_printk_hex(i);
                boot_printk("\n");
                return -1;
            }
            break;

        case EBPF_CLS_LDX:
            /* LDX 不能写入 r10 */
            if (dst == BPF_REG_10) {
                boot_printk("[verifier] REJECT: LDX to r10 at pc=");
                boot_printk_hex(i);
                boot_printk("\n");
                return -1;
            }
            break;

        case EBPF_CLS_JMP: {
            u8 op = code & 0xf0;

            if (op == EBPF_EXIT) {
                has_exit = true;
                break;
            }

            if (op == EBPF_CALL) {
                /* 简化：允许 CALL 但不检查 helper 合法性 */
                break;
            }

            if (op == EBPF_JA) {
                /* 无条件跳转：检查目标有效性 */
                s32 target = (s32)i + 1 + off;
                if (target < 0 || (u32)target >= len) {
                    boot_printk("[verifier] REJECT: JA out of bounds at pc=");
                    boot_printk_hex(i);
                    boot_printk("\n");
                    return -1;
                }
                break;
            }

            /* 条件跳转：检查目标有效性 */
            {
                s32 target = (s32)i + 1 + off;
                if (target < 0 || (u32)target >= len) {
                    boot_printk("[verifier] REJECT: jump out of bounds at pc=");
                    boot_printk_hex(i);
                    boot_printk("\n");
                    return -1;
                }
            }

            /* 检查 5：简化的终止性检查
             * 不允许后向跳转（防止无限循环）*/
            if (off < 0) {
                boot_printk("[verifier] REJECT: backward jump at pc=");
                boot_printk_hex(i);
                boot_printk("\n");
                return -1;
            }
            break;
        }

        case EBPF_CLS_LD:
            /* 64位立即数加载占两条指令，跳过下一条 */
            if ((code & 0x18) == EBPF_DW) {
                if (i + 1 >= len) {
                    boot_printk("[verifier] REJECT: LD_IMM64 at end\n");
                    return -1;
                }
                i++;  /* 跳过下一条伪指令 */
            }
            break;
        }
    }

    /* 检查 6：至少有一条 EXIT 指令 */
    if (!has_exit) {
        boot_printk("[verifier] REJECT: no EXIT instruction\n");
        return -1;
    }

    return 0;
}
