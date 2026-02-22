# Phase 1：ARM64启动与异常向量

## 参考内核文件

```
arch/arm64/kernel/head.S          # 内核启动入口（必读）
arch/arm64/kernel/entry.S         # 异常向量表（必读）
arch/arm64/kernel/proc.S          # CPU初始化、TLB/Cache操作
```

---

## 1.1 启动流程（参考 head.S）

QEMU virt machine 将内核加载到物理地址 `0x40080000`，随后跳转执行。
启动必须满足 ARM64 Linux Boot Protocol：
- `x0` = FDT (Flat Device Tree) 物理地址
- `x1` = `x2` = `x3` = 0（保留）
- CPU处于EL2或EL1，大端/小端取决于配置（我们用小端LE）

**Phase 1 说明：本阶段不启用MMU，直接在物理地址运行。MMU初始化和高地址虚拟映射留给 Phase 2。**

```
Phase 1 头.S 关键步骤顺序：
1. 关闭中断（daifset）
2. 保存 boot_args（x0-x3）
3. 检查当前EL，若为EL2则降级到EL1
4. 设置初始栈指针（init_stack_top）
5. 清零 BSS 段
6. 安装异常向量表（msr vbar_el1）
7. 调用 start_kernel()

注：__create_page_tables / __cpu_setup / __enable_mmu 属于 Phase 2 内容，
    本阶段不实现。
```

## 1.2 链接脚本（linker.ld）

```ld
/* 参考 arch/arm64/kernel/vmlinux.lds.S */
/*
 * Phase 1：不启用MMU，链接到物理地址 0x40080000。
 * Phase 2 将切换到高虚拟地址 0xFFFF000040080000（启用MMU后的内核空间）。
 */
OUTPUT_ARCH(aarch64)
ENTRY(_start)

SECTIONS {
    /* 物理地址：QEMU virt machine 将内核加载到此处 */
    . = 0x40080000;

    .head.text : {
        _text = .;
        KEEP(*(.head.text))   /* head.S 必须在最前面 */
    }

    .text : {
        *(.text .text.*)
    }

    . = ALIGN(4096);
    .rodata : { *(.rodata .rodata.*) }

    . = ALIGN(4096);
    .data : {
        _data = .;
        *(.data .data.*)
    }

    . = ALIGN(16);
    _bss_start = .;
    .bss (NOLOAD) : {
        *(.bss .bss.*)
        *(COMMON)
    }
    . = ALIGN(16);
    _bss_end = .;

    _end = .;
}
```

## 1.3 head.S 实现要点

```asm
/* 参考 arch/arm64/kernel/head.S */
/* ARM64 Linux Image Header — 必须与 bootloader 约定一致 */
.section ".head.text", "ax"
_head:
    /* 跳转到真正入口，跳过 Image Header 数据 */
    b       _start
    .quad   0                       /* Image load offset from start of RAM */
    .quad   0                       /* Image effective size */
    .quad   0                       /* flags */
    .quad   0, 0, 0                 /* reserved */
    .ascii  "ARM\x64"               /* magic */
    .long   0                       /* reserved (PE header offset) */

.section ".text", "ax"
_start:
    /* 1. 关闭所有中断和调试异常 */
    msr     daifset, #0xf

    /* 2. 保存启动参数（x0=FDT地址，由QEMU传入）*/
    adr     x21, boot_args
    str     x0,  [x21]
    str     x1,  [x21, #8]
    str     x2,  [x21, #16]
    str     x3,  [x21, #24]

    /* 3. 检查当前EL */
    mrs     x0, CurrentEL
    and     x0, x0, #0xc            /* bits[3:2] = EL 值 */
    cmp     x0, #8                  /* EL2? */
    beq     from_el2
    b       setup_el1               /* EL1: 直接设置 */

from_el2:
    /* 配置HCR_EL2，降级到EL1（AArch64模式）*/
    mov     x0, #(1 << 31)         /* HCR_EL2.RW = 1: EL1 使用 AArch64 */
    msr     hcr_el2, x0
    /* SCTLR_EL1 复位：关闭MMU/Cache */
    msr     sctlr_el1, xzr
    isb
    /* SPSR_EL2: EL1h 模式，所有中断屏蔽 */
    mov     x0, #0x3c5             /* DAIF=1111, M[4:0]=EL1h */
    msr     spsr_el2, x0
    adr     x0, setup_el1
    msr     elr_el2, x0
    eret

setup_el1:
    /* 4. 设置初始栈（8KB，向下增长）*/
    adr     x0, init_stack_top
    mov     sp, x0

    /* 5. 清零 BSS 段（16字节对齐步进）*/
    adr     x0, _bss_start
    adr     x1, _bss_end
1:
    cmp     x0, x1
    b.ge    2f
    stp     xzr, xzr, [x0], #16
    b       1b
2:
    /* 6. 安装异常向量表（Phase 1 核心：设置 VBAR_EL1）*/
    adr     x0, vectors
    msr     vbar_el1, x0
    isb

    /* 7. 跳转到C入口 start_kernel() */
    bl      start_kernel

    /* 不应返回 */
    b       .

/* Phase 2 将在此处添加：
 *   __create_page_tables  — 建立早期页表（identity map + kernel map）
 *   __cpu_setup           — 初始化CPU：关闭MMU/Cache，设置SCTLR_EL1
 *   __enable_mmu          — 写TTBR0/TTBR1，使能MMU，跳转到虚拟地址
 */
```

## 1.4 异常向量表（参考 entry.S）

ARMv8 异常向量表有 **4×4 = 16** 个入口，每个入口占 128 字节：

```
4 种来源：
  - 当前EL，使用SP_EL0（SP_EL0）
  - 当前EL，使用SP_ELx（SP_ELn）
  - 低EL，AArch64
  - 低EL，AArch32

4 种类型：
  - Synchronous（同步异常：data abort, instruction abort, SVC等）
  - IRQ
  - FIQ
  - SError
```

```asm
/* 参考 arch/arm64/kernel/entry.S */
/* 向量表必须 2KB 对齐（.align 11 = 2^11 = 2048字节）*/
.align 11
vectors:
    /* 当前EL, SP_EL0 */
    ventry  el1t_sync
    ventry  el1t_irq
    ventry  el1t_fiq
    ventry  el1t_error

    /* 当前EL, SP_EL1（内核态中断/异常）*/
    ventry  el1h_sync
    ventry  el1h_irq       /* 内核态IRQ — 最常用 */
    ventry  el1h_fiq
    ventry  el1h_error

    /* 低EL (用户态), AArch64 */
    ventry  el0_sync        /* 系统调用 SVC 走这里 */
    ventry  el0_irq
    ventry  el0_fiq
    ventry  el0_error

    /* 低EL (用户态), AArch32 (我们不支持，填异常处理) */
    ventry  el0_32_sync
    ventry  el0_32_irq
    ventry  el0_32_fiq
    ventry  el0_32_error

/* 宏：每个向量入口占 128 字节（.align 7 = 2^7）*/
.macro ventry label
    .align 7
    b       \label
.endm

/* 宏：保存所有寄存器上下文（pt_regs结构体）
 *
 * pt_regs 布局（272字节 = 34×8）：
 *   offset   0: x0  .. offset 232: x29
 *   offset 240: x30 (LR)
 *   offset 248: SP（EL0，用户态异常时保存）
 *   offset 256: PC（ELR_EL1，异常返回地址）
 *   offset 264: PSTATE（SPSR_EL1）
 */
#define PT_REGS_SIZE    272
#define PT_LR           (30 * 8)    /* 240 */
#define PT_SP           (31 * 8)    /* 248 */
#define PT_PC           (32 * 8)    /* 256 */
#define PT_PSTATE       (33 * 8)    /* 264 */

.macro  kernel_entry
    sub     sp, sp, #PT_REGS_SIZE
    stp     x0,  x1,  [sp, #16 * 0]
    stp     x2,  x3,  [sp, #16 * 1]
    stp     x4,  x5,  [sp, #16 * 2]
    stp     x6,  x7,  [sp, #16 * 3]
    stp     x8,  x9,  [sp, #16 * 4]
    stp     x10, x11, [sp, #16 * 5]
    stp     x12, x13, [sp, #16 * 6]
    stp     x14, x15, [sp, #16 * 7]
    stp     x16, x17, [sp, #16 * 8]
    stp     x18, x19, [sp, #16 * 9]
    stp     x20, x21, [sp, #16 * 10]
    stp     x22, x23, [sp, #16 * 11]
    stp     x24, x25, [sp, #16 * 12]
    stp     x26, x27, [sp, #16 * 13]
    stp     x28, x29, [sp, #16 * 14]
    str     x30,      [sp, #PT_LR]
    /* 保存ELR（返回地址）、SPSR（状态寄存器）*/
    mrs     x21, elr_el1
    mrs     x22, spsr_el1
    stp     x21, x22, [sp, #PT_PC]
.endm

.macro  kernel_exit
    ldp     x21, x22, [sp, #PT_PC]
    msr     elr_el1,  x21
    msr     spsr_el1, x22
    ldp     x0,  x1,  [sp, #16 * 0]
    ldp     x2,  x3,  [sp, #16 * 1]
    ldp     x4,  x5,  [sp, #16 * 2]
    ldp     x6,  x7,  [sp, #16 * 3]
    ldp     x8,  x9,  [sp, #16 * 4]
    ldp     x10, x11, [sp, #16 * 5]
    ldp     x12, x13, [sp, #16 * 6]
    ldp     x14, x15, [sp, #16 * 7]
    ldp     x16, x17, [sp, #16 * 8]
    ldp     x18, x19, [sp, #16 * 9]
    ldp     x20, x21, [sp, #16 * 10]
    ldp     x22, x23, [sp, #16 * 11]
    ldp     x24, x25, [sp, #16 * 12]
    ldp     x26, x27, [sp, #16 * 13]
    ldp     x28, x29, [sp, #16 * 14]
    ldr     x30,      [sp, #PT_LR]
    add     sp, sp, #PT_REGS_SIZE
    eret
.endm
```

## 1.5 关键系统寄存器（参考 sysreg.h）

```c
/* ARMv8 核心系统寄存器 — 必须掌握 */

// SCTLR_EL1: 系统控制寄存器
// bit 0 (M)  = MMU使能
// bit 2 (C)  = Data Cache使能
// bit 12 (I) = Instruction Cache使能
// bit 25 (EE) = 大/小端（0=小端LE）

// TCR_EL1: 地址翻译控制寄存器
// T0SZ[5:0]  = 用户空间地址位数 (64-T0SZ=VA bits)
// T1SZ[21:16]= 内核空间地址位数
// TG0[15:14] = TTBR0页大小 (00=4KB, 01=64KB, 10=16KB)
// TG1[31:30] = TTBR1页大小
// IPS[34:32] = 物理地址空间大小

// TTBR0_EL1: 用户空间页表基址
// TTBR1_EL1: 内核空间页表基址（高地址空间）
// VBAR_EL1:  异常向量表基址（必须2KB对齐）
```

## 1.6 验证方法

```bash
# 编译
make -C arm64os

# 运行
qemu-system-aarch64 \
    -M virt \
    -cpu cortex-a72 \
    -m 1G \
    -kernel arm64os/arm64os.elf \
    -nographic \
    -serial stdio

# 预期输出（Phase 1 无MMU，物理地址运行）：
# [BOOT] ARM64 kernel starting...
# [BOOT] Exception vectors installed
# [BOOT] Running at EL1
# [BOOT] start_kernel() reached
```

## 1.7 本阶段产出文件

```
arm64os/
├── arch/arm64/kernel/head.S      ← 启动入口（核心）
├── arch/arm64/kernel/entry.S     ← 异常向量表（核心）
├── arch/arm64/include/asm/
│   └── sysreg.h                  ← 系统寄存器定义
├── include/linux/
│   └── types.h                   ← 基础类型定义
├── kernel/
│   ├── printk.c                  ← 串口调试输出（PL011 UART @ 0x09000000）
│   └── main.c                    ← start_kernel() 入口，异常处理桩函数
├── scripts/linker.ld             ← 链接脚本（Phase 1: 物理地址 0x40080000）
└── Makefile                      ← 构建系统（aarch64-linux-gnu-gcc）
```
