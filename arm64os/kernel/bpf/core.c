/* SPDX-License-Identifier: GPL-2.0 */
/*
 * arm64os/kernel/bpf/core.c
 *
 * eBPF 解释器与 JIT 调度
 *
 * 参考：kernel/bpf/core.c
 *
 * Phase 12 实现：
 *   - bpf_init()：初始化 eBPF 子系统（程序池）
 *   - bpf_prog_run_interp()：eBPF 解释器（64位 RISC 指令集）
 *   - bpf_prog_run()：执行入口（自动选择 JIT 或解释器）
 *
 * eBPF 执行链路：
 *   用户态程序 → bpf(BPF_PROG_LOAD) → verifier → JIT → 执行
 *
 * eBPF 寄存器模型（64位）：
 *   r0  = 返回值
 *   r1-r5 = 函数参数
 *   r6-r9 = callee-saved
 *   r10 = 只读栈帧指针
 */

#include <linux/types.h>
#include <linux/bpf.h>

/* 外部函数 */
void boot_printk(const char *s);
void boot_printk_hex(unsigned long val);

/* JIT 全局开关 */
int bpf_jit_enable = 0;

/* eBPF 程序池 */
static struct ebpf_prog prog_pool[MAX_EBPF_PROGS];

/*
 * bpf_init - 初始化 eBPF 子系统
 */
void bpf_init(void)
{
    int i;
    for (i = 0; i < MAX_EBPF_PROGS; i++) {
        prog_pool[i].used = 0;
        prog_pool[i].jit_done = false;
        prog_pool[i].jit_image = NULL;
        prog_pool[i].jit_size = 0;
        prog_pool[i].len = 0;
    }
    boot_printk("[bpf] eBPF subsystem initialized\n");
}

/*
 * bpf_prog_alloc - 从池中分配一个 eBPF 程序槽
 *
 * 返回程序索引（作为 fd），或 -1 表示失败。
 */
int bpf_prog_alloc(void)
{
    int i;
    for (i = 0; i < MAX_EBPF_PROGS; i++) {
        if (!prog_pool[i].used) {
            prog_pool[i].used = 1;
            prog_pool[i].jit_done = false;
            prog_pool[i].jit_image = NULL;
            prog_pool[i].jit_size = 0;
            prog_pool[i].len = 0;
            return i;
        }
    }
    return -1;
}

/*
 * bpf_prog_get - 根据 fd（索引）查找程序
 */
struct ebpf_prog *bpf_prog_get(int fd)
{
    if (fd < 0 || fd >= MAX_EBPF_PROGS)
        return NULL;
    if (!prog_pool[fd].used)
        return NULL;
    return &prog_pool[fd];
}

/* ============================================================
 * bpf_prog_run_interp - eBPF 解释器
 *
 * 执行 eBPF 64位 RISC 指令集。
 *
 * 寄存器：
 *   regs[0..10]：r0-r10（u64）
 *   r10 初始化为栈帧指针（指向 stack[BPF_STACK_SIZE]）
 *
 * ctx 参数通过 r1 传入（程序可通过 r1 访问输入数据）。
 *
 * 返回 r0 的值。
 *
 * 参考：kernel/bpf/core.c ___bpf_prog_run()
 * ============================================================ */
u64 bpf_prog_run_interp(const struct ebpf_prog *prog, const void *ctx)
{
    u64 regs[MAX_BPF_REG];
    u8 stack[BPF_STACK_SIZE];
    const struct bpf_insn *insns = prog->insns;
    u32 len = prog->len;
    u32 pc;
    int i;

    /* 初始化寄存器 */
    for (i = 0; i < MAX_BPF_REG; i++)
        regs[i] = 0;

    /* r1 = 上下文指针 */
    regs[BPF_REG_1] = (u64)(unsigned long)ctx;

    /* r10 = 栈帧指针（栈顶，栈向低地址生长）*/
    regs[BPF_REG_10] = (u64)(unsigned long)(stack + BPF_STACK_SIZE);

    for (pc = 0; pc < len; pc++) {
        const struct bpf_insn *insn = &insns[pc];
        u8 code = insn->code;
        u8 dst = insn->dst_reg;
        u8 src = insn->src_reg;
        s16 off = insn->off;
        s32 imm = insn->imm;

        u8 cls = code & 0x07;
        u8 op  = code & 0xf0;
        u8 s   = code & 0x08;  /* src flag: K=0, X=8 */

        switch (cls) {
        /* ---- ALU64 指令 ---- */
        case EBPF_CLS_ALU64: {
            u64 src_val = s ? regs[src] : (u64)(s64)imm;

            switch (op) {
            case EBPF_ADD:
                regs[dst] += src_val;
                break;
            case EBPF_SUB:
                regs[dst] -= src_val;
                break;
            case EBPF_MUL:
                regs[dst] *= src_val;
                break;
            case EBPF_DIV:
                if (src_val == 0)
                    return 0;
                regs[dst] /= src_val;
                break;
            case EBPF_OR:
                regs[dst] |= src_val;
                break;
            case EBPF_AND:
                regs[dst] &= src_val;
                break;
            case EBPF_LSH:
                regs[dst] <<= (u32)src_val;
                break;
            case EBPF_RSH:
                regs[dst] >>= (u32)src_val;
                break;
            case EBPF_NEG:
                regs[dst] = -(s64)regs[dst];
                break;
            case EBPF_MOD:
                if (src_val == 0)
                    return 0;
                regs[dst] %= src_val;
                break;
            case EBPF_XOR:
                regs[dst] ^= src_val;
                break;
            case EBPF_MOV:
                regs[dst] = src_val;
                break;
            case EBPF_ARSH:
                regs[dst] = (u64)((s64)regs[dst] >> (u32)src_val);
                break;
            }
            break;
        }

        /* ---- ALU32 指令（32位）---- */
        case EBPF_CLS_ALU: {
            u32 src_val32 = s ? (u32)regs[src] : (u32)imm;

            switch (op) {
            case EBPF_ADD:
                regs[dst] = (u32)((u32)regs[dst] + src_val32);
                break;
            case EBPF_SUB:
                regs[dst] = (u32)((u32)regs[dst] - src_val32);
                break;
            case EBPF_MUL:
                regs[dst] = (u32)((u32)regs[dst] * src_val32);
                break;
            case EBPF_DIV:
                if (src_val32 == 0)
                    return 0;
                regs[dst] = (u32)((u32)regs[dst] / src_val32);
                break;
            case EBPF_OR:
                regs[dst] = (u32)((u32)regs[dst] | src_val32);
                break;
            case EBPF_AND:
                regs[dst] = (u32)((u32)regs[dst] & src_val32);
                break;
            case EBPF_LSH:
                regs[dst] = (u32)((u32)regs[dst] << src_val32);
                break;
            case EBPF_RSH:
                regs[dst] = (u32)((u32)regs[dst] >> src_val32);
                break;
            case EBPF_NEG:
                regs[dst] = (u32)(-(s32)regs[dst]);
                break;
            case EBPF_MOD:
                if (src_val32 == 0)
                    return 0;
                regs[dst] = (u32)((u32)regs[dst] % src_val32);
                break;
            case EBPF_XOR:
                regs[dst] = (u32)((u32)regs[dst] ^ src_val32);
                break;
            case EBPF_MOV:
                regs[dst] = (u32)src_val32;
                break;
            }
            break;
        }

        /* ---- 内存加载 (LDX) ---- */
        case EBPF_CLS_LDX: {
            u64 addr = regs[src] + (s64)off;
            u8 size = code & 0x18;

            switch (size) {
            case 0x00:  /* BPF_W: 32位 */
                regs[dst] = *(u32 *)(unsigned long)addr;
                break;
            case 0x08:  /* BPF_H: 16位 */
                regs[dst] = *(u16 *)(unsigned long)addr;
                break;
            case 0x10:  /* BPF_B: 8位 */
                regs[dst] = *(u8 *)(unsigned long)addr;
                break;
            case 0x18:  /* BPF_DW: 64位 */
                regs[dst] = *(u64 *)(unsigned long)addr;
                break;
            }
            break;
        }

        /* ---- 内存存储（立即数 ST）---- */
        case EBPF_CLS_ST: {
            u64 addr = regs[dst] + (s64)off;
            u8 size = code & 0x18;

            switch (size) {
            case 0x00:
                *(u32 *)(unsigned long)addr = (u32)imm;
                break;
            case 0x08:
                *(u16 *)(unsigned long)addr = (u16)imm;
                break;
            case 0x10:
                *(u8 *)(unsigned long)addr = (u8)imm;
                break;
            case 0x18:
                *(u64 *)(unsigned long)addr = (u64)(s64)imm;
                break;
            }
            break;
        }

        /* ---- 内存存储（寄存器 STX）---- */
        case EBPF_CLS_STX: {
            u64 addr = regs[dst] + (s64)off;
            u8 size = code & 0x18;

            switch (size) {
            case 0x00:
                *(u32 *)(unsigned long)addr = (u32)regs[src];
                break;
            case 0x08:
                *(u16 *)(unsigned long)addr = (u16)regs[src];
                break;
            case 0x10:
                *(u8 *)(unsigned long)addr = (u8)regs[src];
                break;
            case 0x18:
                *(u64 *)(unsigned long)addr = regs[src];
                break;
            }
            break;
        }

        /* ---- 跳转指令 ---- */
        case EBPF_CLS_JMP: {
            u64 src_val = s ? regs[src] : (u64)(s64)imm;
            bool taken = false;

            switch (op) {
            case EBPF_EXIT:
                return regs[BPF_REG_0];

            case EBPF_JA:
                pc += off;
                continue;

            case EBPF_CALL:
                /* 简化：不支持 helper 函数调用，返回 0 */
                regs[BPF_REG_0] = 0;
                break;

            case EBPF_JEQ:
                taken = (regs[dst] == src_val);
                break;
            case EBPF_JNE:
                taken = (regs[dst] != src_val);
                break;
            case EBPF_JGT:
                taken = (regs[dst] > src_val);
                break;
            case EBPF_JGE:
                taken = (regs[dst] >= src_val);
                break;
            case EBPF_JSET:
                taken = (regs[dst] & src_val) != 0;
                break;
            case EBPF_JSGT:
                taken = ((s64)regs[dst] > (s64)src_val);
                break;
            case EBPF_JSGE:
                taken = ((s64)regs[dst] >= (s64)src_val);
                break;
            case EBPF_JLT:
                taken = (regs[dst] < src_val);
                break;
            case EBPF_JLE:
                taken = (regs[dst] <= src_val);
                break;
            }

            if (taken)
                pc += off;
            break;
        }

        /* ---- 64位立即数加载（占两条指令）---- */
        case EBPF_CLS_LD: {
            if ((code & 0x18) == EBPF_DW) {
                /* BPF_LD_IMM64: dst = imm64 (lower32 in this insn, upper32 in next) */
                u64 val = (u32)imm;
                if (pc + 1 < len) {
                    pc++;
                    val |= ((u64)(u32)insns[pc].imm) << 32;
                }
                regs[dst] = val;
            }
            break;
        }

        default:
            /* 未知指令类别 */
            boot_printk("[bpf] unknown insn class: ");
            boot_printk_hex(cls);
            boot_printk("\n");
            return 0;
        }
    }

    return regs[BPF_REG_0];
}

/* ============================================================
 * bpf_prog_run - 执行 eBPF 程序（JIT 或解释器）
 *
 * 如果程序已 JIT 编译且 bpf_jit_enable 为 1，调用 JIT 机器码；
 * 否则使用解释器执行。
 *
 * 参考：include/linux/filter.h bpf_prog_run()
 * ============================================================ */
u64 bpf_prog_run(const struct ebpf_prog *prog, const void *ctx)
{
    if (!prog || prog->len == 0)
        return 0;

    if (bpf_jit_enable && prog->jit_done && prog->jit_image) {
        /*
         * JIT 执行：将 JIT 机器码作为函数指针调用。
         *
         * 函数签名：u64 (*)(const void *ctx)
         * ctx 通过 x0 传入（ARM64 ABI），返回值在 x0。
         */
        typedef u64 (*bpf_jit_fn)(const void *);
        bpf_jit_fn fn = (bpf_jit_fn)prog->jit_image;
        return fn(ctx);
    }

    /* 回退到解释器 */
    return bpf_prog_run_interp(prog, ctx);
}
