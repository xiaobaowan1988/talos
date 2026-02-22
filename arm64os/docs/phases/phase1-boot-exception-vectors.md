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

```
参考 head.S 中的关键步骤顺序：
1. preserve_boot_args       → 保存 x0-x3 到 boot_args[]
2. el2_setup                → 从EL2降级到EL1（如需要）
3. set_cpu_boot_mode_flag   → 记录启动时EL
4. __create_page_tables     → 建立早期页表（identity map + kernel map）
5. __cpu_setup              → 初始化CPU：关闭MMU/Cache，设置SCTLR_EL1
6. __enable_mmu             → 写TTBR0/TTBR1，使能MMU
7. __primary_switched       → 跳转到虚拟地址，清零BSS，调用 start_kernel()
```

## 1.2 链接脚本（linker.ld）

```ld
/* 参考 arch/arm64/kernel/vmlinux.lds.S */
OUTPUT_ARCH(aarch64)
ENTRY(_start)

SECTIONS {
    /* 内核加载到 0x40080000 物理地址 */
    /* 虚拟地址从 0xFFFF000000000000 开始（高地址内核空间）*/
    . = 0xFFFF000040080000;

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

    . = ALIGN(4096);
    .bss (NOLOAD) : {
        _bss_start = .;
        *(.bss .bss.*)
        *(COMMON)
        _bss_end = .;
    }

    _end = .;
}
```

## 1.3 head.S 实现要点

```asm
/* 参考 arch/arm64/kernel/head.S */
/* ARM64 Linux Image Header — 必须与 bootloader 约定一致 */
.section ".head.text", "ax"
_head:
    /* 魔数 & 偏移，QEMU直接加载可省略 */
    b       _start
    .quad   0                       /* Image load offset from start of RAM */
    .quad   _end - _head            /* Image size */
    .quad   0                       /* flags */
    .quad   0, 0, 0                 /* reserved */
    .ascii  "ARM\x64"               /* magic */
    .long   0                       /* reserved (PE header offset) */

_start:
    /* 1. 关闭中断，设置初始栈指针 */
    msr     daifset, #0xf
    adr     x0, init_stack_top
    mov     sp, x0

    /* 2. 检查当前EL，如果是EL2则降级 */
    mrs     x0, CurrentEL
    cmp     x0, #(2 << 2)
    beq     from_el2

    /* 3. 保存dtb地址（x0寄存器在启动时由QEMU传入）*/
    adr     x21, boot_args
    str     x20, [x21]              /* x20 = dtb物理地址（启动前保存）*/

    /* 4. 建立页表、开启MMU → 见 Phase 2 */
    bl      __create_page_tables
    bl      __cpu_setup
    bl      __enable_mmu

    /* 5. 跳转到C入口 */
    bl      start_kernel

from_el2:
    /* 配置HCR_EL2，降级到EL1 */
    mov     x0, #(1 << 31)         /* HCR_EL2.RW = 1 (AArch64 EL1) */
    msr     hcr_el2, x0
    mov     x0, #0x3c5             /* SPSR_EL2: EL1h mode */
    msr     spsr_el2, x0
    adr     x0, el1_entry
    msr     elr_el2, x0
    eret
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
/* 向量表必须 2KB 对齐 */
.align 11
vectors:
    /* 当前EL, SP_EL0 */
    kernel_ventry   el1t, sync
    kernel_ventry   el1t, irq
    kernel_ventry   el1t, fiq
    kernel_ventry   el1t, error

    /* 当前EL, SP_EL1（内核态中断/异常） */
    kernel_ventry   el1h, sync
    kernel_ventry   el1h, irq       /* 内核态IRQ — 最常用 */
    kernel_ventry   el1h, fiq
    kernel_ventry   el1h, error

    /* 低EL (用户态), AArch64 */
    kernel_ventry   el0_64, sync    /* 系统调用 SVC 走这里 */
    kernel_ventry   el0_64, irq
    kernel_ventry   el0_64, fiq
    kernel_ventry   el0_64, error

    /* 低EL (用户态), AArch32 (我们不支持，填异常处理) */
    kernel_ventry   el0_32, sync
    kernel_ventry   el0_32, irq
    kernel_ventry   el0_32, fiq
    kernel_ventry   el0_32, error

/* 宏：保存所有寄存器上下文（pt_regs结构体）*/
.macro  kernel_entry, el
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
    /* 保存ELR（返回地址）、SPSR（状态寄存器）*/
    mrs     x21, elr_el1
    mrs     x22, spsr_el1
    stp     x30, x21, [sp, #PT_LR]
    str     x22, [sp, #PT_PSTATE]
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
# 编译并运行
qemu-system-aarch64 \
    -M virt \
    -cpu cortex-a72 \
    -m 1G \
    -kernel arm64os.elf \
    -nographic \
    -serial stdio

# 预期输出：
# [BOOT] ARM64 kernel starting...
# [BOOT] Exception vectors installed at 0xFFFF000040090000
# [BOOT] Running at EL1
# start_kernel() reached
```

## 1.7 本阶段产出文件

```
arm64os/
├── arch/arm64/kernel/head.S      ← 启动入口（核心）
├── arch/arm64/kernel/entry.S     ← 异常向量表（核心）
├── arch/arm64/include/asm/
│   └── sysreg.h                  ← 系统寄存器定义
├── kernel/printk.c               ← 串口调试输出
└── scripts/linker.ld             ← 链接脚本
```
