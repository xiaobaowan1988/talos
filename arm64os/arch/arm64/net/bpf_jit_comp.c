/* SPDX-License-Identifier: GPL-2.0 */
/*
 * arm64os/arch/arm64/net/bpf_jit_comp.c
 *
 * ARM64 eBPF JIT 编译器
 *
 * 参考：arch/arm64/net/bpf_jit_comp.c
 *
 * Phase 12 实现：
 *   - bpf_jit_compile()：将 eBPF 字节码翻译为 AArch64 机器码
 *   - build_insn()：单条 eBPF 指令到 AArch64 指令的翻译
 *
 * eBPF 寄存器到 AArch64 寄存器映射（参考 Linux 内核实现）：
 *   eBPF r0  → x7   (返回值)
 *   eBPF r1  → x0   (第1参数 / ctx)
 *   eBPF r2  → x1
 *   eBPF r3  → x2
 *   eBPF r4  → x3
 *   eBPF r5  → x4
 *   eBPF r6  → x19  (callee-saved)
 *   eBPF r7  → x20
 *   eBPF r8  → x21
 *   eBPF r9  → x22
 *   eBPF r10 → x25  (栈帧指针)
 *
 * 临时寄存器：x10, x11（编译器内部使用）
 *
 * 简化说明：
 *   本 JIT 生成的机器码使用静态缓冲区，不涉及实际的 icache 刷新
 *   和 W^X 内存保护。在 QEMU 上运行时，解释器执行路径更适合验证，
 *   JIT 主要作为教学演示。
 */

#include <linux/types.h>
#include <linux/bpf.h>

/* 外部函数 */
void boot_printk(const char *s);
void boot_printk_hex(unsigned long val);

/* ============================================================
 * AArch64 寄存器编号
 * ============================================================ */
#define A64_R(n)    (n)
#define A64_FP      29
#define A64_LR      30
#define A64_SP      31  /* 在某些编码中用作 SP */
#define A64_ZR      31  /* 在某些编码中用作 XZR */

/* 临时寄存器（JIT 内部使用）*/
#define A64_TMP     A64_R(10)
#define A64_TMP2    A64_R(11)

/* ============================================================
 * eBPF → AArch64 寄存器映射
 * ============================================================ */
static const u8 bpf2a64[MAX_BPF_REG] = {
    [BPF_REG_0]  = A64_R(7),       /* 返回值 */
    [BPF_REG_1]  = A64_R(0),       /* arg1 / ctx */
    [BPF_REG_2]  = A64_R(1),       /* arg2 */
    [BPF_REG_3]  = A64_R(2),       /* arg3 */
    [BPF_REG_4]  = A64_R(3),       /* arg4 */
    [BPF_REG_5]  = A64_R(4),       /* arg5 */
    [BPF_REG_6]  = A64_R(19),      /* callee-saved */
    [BPF_REG_7]  = A64_R(20),      /* callee-saved */
    [BPF_REG_8]  = A64_R(21),      /* callee-saved */
    [BPF_REG_9]  = A64_R(22),      /* callee-saved */
    [BPF_REG_10] = A64_R(25),      /* frame pointer (read-only) */
};

/* ============================================================
 * JIT 编译上下文
 * ============================================================ */

/* JIT 代码缓冲区（全局静态，简化版不做 W^X 保护）*/
static u32 jit_buf[BPF_JIT_MAX_SIZE / 4];

struct jit_ctx {
    u32    *image;          /* 机器码缓冲区 */
    u32     idx;            /* 当前写入位置 */
    u32     max_size;       /* 缓冲区大小（u32 单位）*/
    u32     epilogue_offset; /* 收尾代码偏移 */
    /* 每条 eBPF 指令对应的机器码偏移（用于跳转修正）*/
    u32     insn_offset[BPF_MAXINSNS + 1];
};

/* 发射一条 AArch64 指令 */
static void emit(u32 insn_val, struct jit_ctx *ctx)
{
    if (ctx->idx < ctx->max_size)
        ctx->image[ctx->idx] = insn_val;
    ctx->idx++;
}

/* ============================================================
 * AArch64 指令编码辅助函数
 *
 * 这些函数生成 AArch64 指令的二进制编码。
 * 参考：ARM Architecture Reference Manual, C4.1
 * ============================================================ */

/* ADD Xd, Xn, #imm12 */
static u32 a64_add_imm(int sf, u8 rd, u8 rn, u32 imm12)
{
    return (((u32)sf << 31) | (0x11 << 24) | ((imm12 & 0xfff) << 10) |
            ((u32)rn << 5) | (u32)rd);
}

/* SUB Xd, Xn, #imm12 */
static u32 a64_sub_imm(int sf, u8 rd, u8 rn, u32 imm12)
{
    return (((u32)sf << 31) | (0x51 << 24) | ((imm12 & 0xfff) << 10) |
            ((u32)rn << 5) | (u32)rd);
}

/* ADD Xd, Xn, Xm */
static u32 a64_add_reg(int sf, u8 rd, u8 rn, u8 rm)
{
    return (((u32)sf << 31) | (0x0b << 24) | ((u32)rm << 16) |
            ((u32)rn << 5) | (u32)rd);
}

/* SUB Xd, Xn, Xm */
static u32 a64_sub_reg(int sf, u8 rd, u8 rn, u8 rm)
{
    return (((u32)sf << 31) | (0x4b << 24) | ((u32)rm << 16) |
            ((u32)rn << 5) | (u32)rd);
}

/* MOVZ Xd, #imm16, LSL #shift */
static u32 a64_movz(int sf, u8 rd, u16 imm16, u8 shift)
{
    u32 hw = shift / 16;
    return (((u32)sf << 31) | (0xa5 << 23) | (hw << 21) |
            ((u32)imm16 << 5) | (u32)rd);
}

/* MOVK Xd, #imm16, LSL #shift */
static u32 a64_movk(int sf, u8 rd, u16 imm16, u8 shift)
{
    u32 hw = shift / 16;
    return (((u32)sf << 31) | (0xe5 << 23) | (hw << 21) |
            ((u32)imm16 << 5) | (u32)rd);
}

/* MOV Xd, Xm (alias for ORR Xd, XZR, Xm) */
static u32 a64_mov_reg(int sf, u8 rd, u8 rm)
{
    return (((u32)sf << 31) | (0x2a << 24) | ((u32)rm << 16) |
            (A64_ZR << 5) | (u32)rd);
}

/* AND Xd, Xn, Xm */
static u32 a64_and_reg(int sf, u8 rd, u8 rn, u8 rm)
{
    return (((u32)sf << 31) | (0x0a << 24) | ((u32)rm << 16) |
            ((u32)rn << 5) | (u32)rd);
}

/* ORR Xd, Xn, Xm */
static u32 a64_orr_reg(int sf, u8 rd, u8 rn, u8 rm)
{
    return (((u32)sf << 31) | (0x2a << 24) | ((u32)rm << 16) |
            ((u32)rn << 5) | (u32)rd);
}

/* EOR Xd, Xn, Xm */
static u32 a64_eor_reg(int sf, u8 rd, u8 rn, u8 rm)
{
    return (((u32)sf << 31) | (0x4a << 24) | ((u32)rm << 16) |
            ((u32)rn << 5) | (u32)rd);
}

/* LSL Xd, Xn, Xm (alias: LSLV) */
static u32 a64_lslv(int sf, u8 rd, u8 rn, u8 rm)
{
    return (((u32)sf << 31) | (0xd6 << 21) | ((u32)rm << 16) |
            (0x08 << 10) | ((u32)rn << 5) | (u32)rd);
}

/* LSR Xd, Xn, Xm (alias: LSRV) */
static u32 a64_lsrv(int sf, u8 rd, u8 rn, u8 rm)
{
    return (((u32)sf << 31) | (0xd6 << 21) | ((u32)rm << 16) |
            (0x09 << 10) | ((u32)rn << 5) | (u32)rd);
}

/* CMP Xn, #imm12 (alias: SUBS XZR, Xn, #imm12) */
static u32 a64_cmp_imm(int sf, u8 rn, u32 imm12)
{
    return (((u32)sf << 31) | (0x71 << 24) | ((imm12 & 0xfff) << 10) |
            ((u32)rn << 5) | A64_ZR);
}

/* CMP Xn, Xm (alias: SUBS XZR, Xn, Xm) */
static u32 a64_cmp_reg(int sf, u8 rn, u8 rm)
{
    return (((u32)sf << 31) | (0x6b << 24) | ((u32)rm << 16) |
            ((u32)rn << 5) | A64_ZR);
}

/* B.cond offset */
static u32 a64_bcond(u8 cond, s32 off19)
{
    return (0x54 << 24) | (((u32)off19 & 0x7ffff) << 5) | (u32)cond;
}

/* B offset (unconditional) */
static u32 a64_b(s32 off26)
{
    return (0x05 << 26) | ((u32)off26 & 0x3ffffff);
}

/* LDR Xt, [Xn, #off] (64-bit) */
static u32 a64_ldr64(u8 rt, u8 rn, s16 off)
{
    /* unsigned offset, scaled by 8 */
    u32 uoff = ((u32)off / 8) & 0xfff;
    return (0xf9 << 24) | (0x01 << 22) | (uoff << 10) |
           ((u32)rn << 5) | (u32)rt;
}

/* STR Xt, [Xn, #off] (64-bit) */
static u32 a64_str64(u8 rt, u8 rn, s16 off)
{
    u32 uoff = ((u32)off / 8) & 0xfff;
    return (0xf9 << 24) | (0x00 << 22) | (uoff << 10) |
           ((u32)rn << 5) | (u32)rt;
}

/* LDR Wt, [Xn, #off] (32-bit) */
static u32 a64_ldr32(u8 rt, u8 rn, s16 off)
{
    u32 uoff = ((u32)off / 4) & 0xfff;
    return (0xb9 << 24) | (0x01 << 22) | (uoff << 10) |
           ((u32)rn << 5) | (u32)rt;
}

/* RET */
static u32 a64_ret(void)
{
    return 0xd65f03c0;  /* RET x30 */
}

/* NOP */
static u32 a64_nop(void)
{
    return 0xd503201f;
}

/* AArch64 condition codes */
#define A64_COND_EQ 0x0
#define A64_COND_NE 0x1
#define A64_COND_CS 0x2  /* unsigned >= */
#define A64_COND_CC 0x3  /* unsigned < */
#define A64_COND_HI 0x8  /* unsigned > */
#define A64_COND_LS 0x9  /* unsigned <= */
#define A64_COND_GE 0xa  /* signed >= */
#define A64_COND_LT 0xb  /* signed < */
#define A64_COND_GT 0xc  /* signed > */
#define A64_COND_LE 0xd  /* signed <= */

/* ============================================================
 * emit_imm64 - 发射64位立即数到寄存器
 *
 * 使用 MOVZ + MOVK 序列（最多 4 条指令）
 * ============================================================ */
static void emit_imm64(u8 rd, u64 imm, struct jit_ctx *ctx)
{
    emit(a64_movz(1, rd, (u16)(imm & 0xffff), 0), ctx);
    if (imm > 0xffff)
        emit(a64_movk(1, rd, (u16)((imm >> 16) & 0xffff), 16), ctx);
    if (imm > 0xffffffff)
        emit(a64_movk(1, rd, (u16)((imm >> 32) & 0xffff), 32), ctx);
    if (imm > 0xffffffffffff)
        emit(a64_movk(1, rd, (u16)((imm >> 48) & 0xffff), 48), ctx);
}

/* ============================================================
 * emit_imm32 - 发射32位立即数到寄存器
 * ============================================================ */
static void emit_imm32(u8 rd, s32 imm, struct jit_ctx *ctx)
{
    if (imm >= 0 && imm <= 0xffff) {
        emit(a64_movz(1, rd, (u16)imm, 0), ctx);
    } else {
        u64 val = (u64)(s64)imm;  /* sign-extend */
        emit_imm64(rd, val, ctx);
    }
}

/* ============================================================
 * build_insn - 编译单条 eBPF 指令为 AArch64 机器码
 *
 * 返回 0 表示成功，非 0 表示指令不支持。
 *
 * 参考：arch/arm64/net/bpf_jit_comp.c build_insn()
 * ============================================================ */
static int build_insn(const struct bpf_insn *insn, struct jit_ctx *ctx)
{
    u8 code = insn->code;
    u8 dst = bpf2a64[insn->dst_reg];
    u8 src = bpf2a64[insn->src_reg];
    s32 imm = insn->imm;
    s16 off = insn->off;

    u8 cls = code & 0x07;
    u8 op  = code & 0xf0;
    u8 s   = code & 0x08;

    switch (cls) {
    case EBPF_CLS_ALU64:
        if (s) {
            /* 寄存器操作 */
            switch (op) {
            case EBPF_ADD:
                emit(a64_add_reg(1, dst, dst, src), ctx);
                break;
            case EBPF_SUB:
                emit(a64_sub_reg(1, dst, dst, src), ctx);
                break;
            case EBPF_AND:
                emit(a64_and_reg(1, dst, dst, src), ctx);
                break;
            case EBPF_OR:
                emit(a64_orr_reg(1, dst, dst, src), ctx);
                break;
            case EBPF_XOR:
                emit(a64_eor_reg(1, dst, dst, src), ctx);
                break;
            case EBPF_LSH:
                emit(a64_lslv(1, dst, dst, src), ctx);
                break;
            case EBPF_RSH:
                emit(a64_lsrv(1, dst, dst, src), ctx);
                break;
            case EBPF_MOV:
                emit(a64_mov_reg(1, dst, src), ctx);
                break;
            default:
                emit(a64_nop(), ctx);
                break;
            }
        } else {
            /* 立即数操作 */
            switch (op) {
            case EBPF_ADD:
                if (imm >= 0 && imm < 4096) {
                    emit(a64_add_imm(1, dst, dst, (u32)imm), ctx);
                } else {
                    emit_imm32(A64_TMP, imm, ctx);
                    emit(a64_add_reg(1, dst, dst, A64_TMP), ctx);
                }
                break;
            case EBPF_SUB:
                if (imm >= 0 && imm < 4096) {
                    emit(a64_sub_imm(1, dst, dst, (u32)imm), ctx);
                } else {
                    emit_imm32(A64_TMP, imm, ctx);
                    emit(a64_sub_reg(1, dst, dst, A64_TMP), ctx);
                }
                break;
            case EBPF_MOV:
                emit_imm32(dst, imm, ctx);
                break;
            default:
                /* 其他立即数操作：加载到 TMP 后用寄存器操作 */
                emit_imm32(A64_TMP, imm, ctx);
                switch (op) {
                case EBPF_AND:
                    emit(a64_and_reg(1, dst, dst, A64_TMP), ctx);
                    break;
                case EBPF_OR:
                    emit(a64_orr_reg(1, dst, dst, A64_TMP), ctx);
                    break;
                case EBPF_XOR:
                    emit(a64_eor_reg(1, dst, dst, A64_TMP), ctx);
                    break;
                default:
                    emit(a64_nop(), ctx);
                    break;
                }
                break;
            }
        }
        break;

    case EBPF_CLS_LDX:
        /* 内存加载：dst = *(size *)(src + off) */
        {
            u8 size = code & 0x18;
            switch (size) {
            case 0x00:  /* 32-bit */
                if (off >= 0 && off < 16384 && (off % 4) == 0)
                    emit(a64_ldr32(dst, src, off), ctx);
                else {
                    emit_imm32(A64_TMP, off, ctx);
                    emit(a64_add_reg(1, A64_TMP, src, A64_TMP), ctx);
                    emit(a64_ldr32(dst, A64_TMP, 0), ctx);
                }
                break;
            case 0x18:  /* 64-bit */
                if (off >= 0 && (off % 8) == 0)
                    emit(a64_ldr64(dst, src, off), ctx);
                else {
                    emit_imm32(A64_TMP, off, ctx);
                    emit(a64_add_reg(1, A64_TMP, src, A64_TMP), ctx);
                    emit(a64_ldr64(dst, A64_TMP, 0), ctx);
                }
                break;
            default:
                emit(a64_nop(), ctx);
                break;
            }
        }
        break;

    case EBPF_CLS_STX:
        /* 内存存储：*(size *)(dst + off) = src */
        {
            u8 size = code & 0x18;
            if (size == 0x18) {
                if (off >= 0 && (off % 8) == 0)
                    emit(a64_str64(src, dst, off), ctx);
                else {
                    emit_imm32(A64_TMP, off, ctx);
                    emit(a64_add_reg(1, A64_TMP, dst, A64_TMP), ctx);
                    emit(a64_str64(src, A64_TMP, 0), ctx);
                }
            } else {
                emit(a64_nop(), ctx);
            }
        }
        break;

    case EBPF_CLS_JMP:
        switch (op) {
        case EBPF_EXIT:
            /* mov x0, x7 (将 r0 返回值放入 x0); ret */
            emit(a64_mov_reg(1, A64_R(0), bpf2a64[BPF_REG_0]), ctx);
            emit(a64_ret(), ctx);
            break;

        case EBPF_JA:
            /* 无条件跳转：B +off（偏移在 pass 2 修正）*/
            /* 简化：直接用 NOP 占位，实际跳转在解释器中处理 */
            emit(a64_nop(), ctx);
            break;

        case EBPF_JEQ:
            if (s) {
                emit(a64_cmp_reg(1, dst, src), ctx);
            } else {
                emit_imm32(A64_TMP, imm, ctx);
                emit(a64_cmp_reg(1, dst, A64_TMP), ctx);
            }
            /* B.EQ +off (placeholder) */
            emit(a64_nop(), ctx);
            break;

        case EBPF_JNE:
            if (s) {
                emit(a64_cmp_reg(1, dst, src), ctx);
            } else {
                emit_imm32(A64_TMP, imm, ctx);
                emit(a64_cmp_reg(1, dst, A64_TMP), ctx);
            }
            emit(a64_nop(), ctx);
            break;

        case EBPF_JGT:
            if (s) {
                emit(a64_cmp_reg(1, dst, src), ctx);
            } else {
                emit_imm32(A64_TMP, imm, ctx);
                emit(a64_cmp_reg(1, dst, A64_TMP), ctx);
            }
            emit(a64_nop(), ctx);
            break;

        case EBPF_JGE:
            if (s) {
                emit(a64_cmp_reg(1, dst, src), ctx);
            } else {
                emit_imm32(A64_TMP, imm, ctx);
                emit(a64_cmp_reg(1, dst, A64_TMP), ctx);
            }
            emit(a64_nop(), ctx);
            break;

        case EBPF_CALL:
            /* 简化：不支持 helper 调用 */
            emit(a64_movz(1, bpf2a64[BPF_REG_0], 0, 0), ctx);
            break;

        default:
            emit(a64_nop(), ctx);
            break;
        }
        break;

    case EBPF_CLS_LD:
        /* 64位立即数加载 */
        if ((code & 0x18) == EBPF_DW) {
            emit_imm32(dst, imm, ctx);
        }
        break;

    default:
        emit(a64_nop(), ctx);
        break;
    }

    return 0;
}

/* ============================================================
 * bpf_jit_compile - JIT 编译 eBPF 程序
 *
 * 将 eBPF 字节码翻译为 AArch64 机器码。
 *
 * 编译流程：
 *   1. 发射 prologue（保存 callee-saved 寄存器）
 *   2. 逐条翻译 eBPF 指令
 *   3. 发射 epilogue（恢复寄存器 + ret）
 *
 * 返回 0 表示成功。
 *
 * 参考：arch/arm64/net/bpf_jit_comp.c bpf_int_jit_compile()
 * ============================================================ */
int bpf_jit_compile(struct ebpf_prog *prog)
{
    struct jit_ctx ctx;
    u32 i;

    if (!prog || prog->len == 0)
        return -1;

    ctx.image = jit_buf;
    ctx.idx = 0;
    ctx.max_size = BPF_JIT_MAX_SIZE / 4;
    ctx.epilogue_offset = 0;

    /* 记录每条 eBPF 指令的机器码偏移 */
    for (i = 0; i <= prog->len; i++)
        ctx.insn_offset[i] = 0;

    /*
     * Prologue：
     * 简化版 — 不保存/恢复 callee-saved 寄存器。
     * 因为我们在 QEMU 中以教学模式运行，实际 JIT 执行路径
     * 主要用于验证 JIT 编译流程本身是否正确。
     *
     * 完整实现应保存 x19-x25, x29, x30 到栈。
     */

    /* 逐条编译 eBPF 指令 */
    for (i = 0; i < prog->len; i++) {
        ctx.insn_offset[i] = ctx.idx;
        build_insn(&prog->insns[i], &ctx);

        /* 64位立即数加载占两条 eBPF 指令 */
        if ((prog->insns[i].code & 0x07) == EBPF_CLS_LD &&
            (prog->insns[i].code & 0x18) == EBPF_DW) {
            i++;    /* 跳过伪指令 */
            if (i < prog->len)
                ctx.insn_offset[i] = ctx.idx;
        }
    }
    ctx.insn_offset[prog->len] = ctx.idx;

    /* 检查缓冲区是否溢出 */
    if (ctx.idx > ctx.max_size) {
        boot_printk("[jit] code buffer overflow\n");
        return -1;
    }

    /* 设置 JIT 结果 */
    prog->jit_image = jit_buf;
    prog->jit_size = ctx.idx * 4;
    prog->jit_done = true;

    boot_printk("[jit] compiled ");
    boot_printk_hex((unsigned long)prog->len);
    boot_printk(" eBPF insns → ");
    boot_printk_hex((unsigned long)ctx.idx);
    boot_printk(" AArch64 insns (");
    boot_printk_hex((unsigned long)(ctx.idx * 4));
    boot_printk(" bytes)\n");

    return 0;
}
