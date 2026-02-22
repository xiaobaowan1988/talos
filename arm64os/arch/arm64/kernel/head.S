/* SPDX-License-Identifier: GPL-2.0 */
/*
 * arm64os/arch/arm64/kernel/head.S
 *
 * ARM64 内核启动入口
 *
 * 参考：arch/arm64/kernel/head.S
 *
 * Phase 1 实现要点：
 *   - 遵循 ARM64 Linux Boot Protocol（x0=FDT, x1=x2=x3=0）
 *   - 处理 EL2 → EL1 降级
 *   - 设置初始内核栈
 *   - 清零 BSS 段
 *   - 安装异常向量表（VBAR_EL1）
 *   - 跳转 start_kernel()
 *
 * Phase 2 将在此基础上添加：
 *   - __create_page_tables（建立页表）
 *   - __cpu_setup（CPU 特性配置）
 *   - __enable_mmu（开启 MMU，跳转到高虚拟地址）
 */

    /*
     * .head.text 节：ARM64 Image 头部，链接脚本保证其在最前面（0x40080000）。
     * QEMU 的 ARM64 loader 通过识别 Image 头部魔数加载内核。
     */
    .section ".head.text", "ax"

    .global _head
_head:
    /*
     * ARM64 Linux Image 头部（64 字节）
     * 格式参考：Documentation/arm64/booting.rst
     *
     * offset  0: b _start           — 第一条指令（bootloader 直接跳入时执行）
     * offset  4: .quad load_offset  — 从 RAM 起始处的加载偏移量
     * offset 12: .quad image_size   — 镜像有效大小
     * offset 20: .quad flags        — 内核标志（bit 0: LE=0/BE=1, bit 1: page_size）
     * offset 28: .quad reserved×3
     * offset 52: .ascii "ARM\x64"  — ARM64 魔数
     * offset 56: .long  pe_header  — PE/COFF 头偏移（可选，0 = 无 PE）
     */
    b       _start                  /* 立即跳转到真正入口 */
    .quad   0x0                     /* Image load offset: 0 = 从 RAM 基址加载 */
    .quad   _end - _head            /* Image effective size */
    .quad   0x0                     /* flags: little-endian, 4KB page */
    .quad   0x0                     /* reserved */
    .quad   0x0                     /* reserved */
    .quad   0x0                     /* reserved */
    .ascii  "ARM\x64"               /* magic: "ARMd" */
    .long   0x0                     /* PE/COFF header offset (0 = no PE) */

/*
 * =============================================================
 * 以下代码在 .text 节中，紧跟 Image 头部之后。
 * =============================================================
 */
    .section ".text", "ax"

    .global _start
/*
 * _start - 内核入口点
 *
 * ARM64 Linux Boot Protocol 规定（参考 Documentation/arm64/booting.rst）：
 *   x0 = 设备树（FDT）物理地址（bootloader 传入，QEMU 自动设置）
 *   x1 = 0（保留）
 *   x2 = 0（保留）
 *   x3 = 0（保留）
 *
 * CPU 初始状态：
 *   - 可能处于 EL2 或 EL1（QEMU virt 通常从 EL2 开始）
 *   - Cache 和 MMU 关闭
 *   - 小端模式（LE）
 */
_start:
    /* ---- Step 1: 关闭所有异常和中断 ---- */
    msr     daifset, #0xf           /* D=debug, A=SError, I=IRQ, F=FIQ 全部屏蔽 */

    /* ---- Step 2: 保存启动参数 ---- */
    /*
     * 必须在其他操作之前保存 x0-x3，因为后续指令可能覆盖这些寄存器。
     * adr 使用 PC 相对寻址，在无 MMU 的物理地址模式下正常工作。
     */
    adr     x21, boot_args
    str     x0,  [x21]             /* boot_args[0] = FDT 物理地址 */
    str     x1,  [x21, #8]         /* boot_args[1] = 保留（应为0）*/
    str     x2,  [x21, #16]        /* boot_args[2] = 保留（应为0）*/
    str     x3,  [x21, #24]        /* boot_args[3] = 保留（应为0）*/

    /* ---- Step 3: 检查当前异常级别 ---- */
    /*
     * CurrentEL 寄存器 bits[3:2] 给出当前 EL：
     *   0b00 = EL0, 0b01 = EL1, 0b10 = EL2, 0b11 = EL3
     * 读出后需左移 2 位（因为 CurrentEL 的 EL 字段在 bits[3:2]，
     * 右移 2 位即可得到 EL 数值）。
     */
    mrs     x0, CurrentEL
    and     x0, x0, #0xc           /* 屏蔽无关位，只看 bits[3:2] */
    cmp     x0, #8                 /* 0x8 = EL2（2 << 2）*/
    beq     from_el2
    cmp     x0, #4                 /* 0x4 = EL1（1 << 2）*/
    beq     setup_el1
    /* EL0 或 EL3：不支持，挂死 */
    b       .

/*
 * from_el2 - 从 EL2 降级到 EL1
 *
 * QEMU virt machine 在 -M virt 下 CPU 启动于 EL2。
 * 我们使用 eret 指令切换到 EL1，继续执行 setup_el1。
 */
from_el2:
    /*
     * HCR_EL2.RW = 1：设置 EL1 为 AArch64 模式（必须！）。
     * 不启用虚拟化（HCR_EL2.VM=0）。
     */
    mov     x0, #(1 << 31)         /* HCR_EL2.RW = 1 */
    msr     hcr_el2, x0

    /*
     * 将 SCTLR_EL1 设为安全的初始值：
     *   - MMU 关闭（M=0）
     *   - Cache 关闭（C=0, I=0）
     * 写入 xzr（全零）即可，这对 Phase 1 足够。
     */
    msr     sctlr_el1, xzr
    isb                             /* 指令同步屏障：确保 MSR 生效 */

    /*
     * SPSR_EL2：返回后进入 EL1h 模式，中断全部屏蔽。
     * 0x3c5 = 0b_0000_0011_1100_0101
     *   M[4:0] = 0b00101 = EL1h（使用 SP_EL1）
     *   DAIF   = 0b1111（D=1, A=1, I=1, F=1 全部屏蔽）
     */
    mov     x0, #0x3c5
    msr     spsr_el2, x0

    /*
     * ELR_EL2：eret 后跳转目标 = setup_el1。
     * adr 获取 setup_el1 的物理地址（PC 相对）。
     */
    adr     x0, setup_el1
    msr     elr_el2, x0

    eret                            /* 切换到 EL1，跳转到 setup_el1 */

/*
 * setup_el1 - EL1 初始化
 *
 * 此时运行于 EL1，但还没有栈、没有 MMU。
 * 完成 Phase 1 必要的初始化：栈、BSS、向量表，然后调用 start_kernel()。
 */
setup_el1:
    /* ---- Step 4: 设置初始内核栈 ---- */
    /*
     * 栈向下增长，sp 指向栈顶（高地址端）。
     * init_stack_top 定义在下方的 .data 节末尾。
     * 16 字节对齐满足 AArch64 ABI 要求。
     */
    adr     x0, init_stack_top
    mov     sp, x0

    /* ---- Step 5: 清零 BSS 段 ---- */
    /*
     * BSS 段包含未初始化的全局变量，C 标准要求初始值为 0。
     * 使用 stp xzr, xzr 每次清 16 字节（对齐步进）。
     * _bss_start 和 _bss_end 由链接脚本定义（均 16 字节对齐）。
     */
    adr     x0, _bss_start
    adr     x1, _bss_end
.Lbss_loop:
    cmp     x0, x1
    b.ge    .Lbss_done
    stp     xzr, xzr, [x0], #16    /* 清 16 字节，x0 += 16 */
    b       .Lbss_loop
.Lbss_done:

    /* ---- Step 6: 安装异常向量表 ---- */
    /*
     * VBAR_EL1：异常向量表基址寄存器。
     * vectors 定义在 entry.S 中，以 .align 11（2KB）对齐。
     * isb 确保下一条指令使用新的向量表。
     */
    adr     x0, vectors
    msr     vbar_el1, x0
    isb

    /* ---- Step 7: 跳转到 C 入口 ---- */
    bl      start_kernel

    /* start_kernel() 不应返回；若返回则挂死 */
    b       .

/*
 * =============================================================
 * 数据节
 * =============================================================
 */
    .section ".data", "aw"

/*
 * boot_args[4]：保存 bootloader 传入的 x0-x3。
 * 8 字节对齐（.align 3 = 2^3 = 8）。
 */
    .global boot_args
    .align  3
boot_args:
    .quad   0       /* x0: FDT 物理地址 */
    .quad   0       /* x1: 保留 */
    .quad   0       /* x2: 保留 */
    .quad   0       /* x3: 保留 */

/*
 * 初始启动栈（8KB）
 *
 * 在 start_kernel() 建立正式进程栈之前，这里是内核的临时栈。
 * 必须 16 字节对齐（AArch64 ABI）。
 * 此处为 BSS 段之前，不受 BSS 清零影响。
 */
    .balign 16
    .skip   8192                    /* 8KB 栈空间 */
init_stack_top:                     /* 栈从此地址向下增长 */
