# Phase 1 Walkthrough: ARM64 启动与异常向量 — 从零到一

> **目标**：让 CPU 从上电到执行第一行 C 代码，并安装异常向量表。
> **最终效果**：QEMU 启动内核，串口打印 `[BOOT] ARM64 kernel starting...`。

---

## 1.1 建立构建环境

### 我们需要什么？

裸机编程没有操作系统，不能用标准库。我们需要一个**交叉编译器**（在 x86 上编译 ARM64 代码）和一个**模拟器**（QEMU）。

### 第一步：创建项目骨架

```
arm64os/
├── arch/arm64/kernel/
│   ├── head.S          # 汇编入口
│   └── entry.S         # 异常向量表
├── kernel/
│   ├── main.c          # C 入口 start_kernel()
│   └── printk.c        # 串口输出
├── include/
│   └── linux/types.h   # 基本类型定义
├── scripts/
│   └── linker.ld       # 链接脚本
└── Makefile            # 构建系统
```

### 第二步：Makefile

```makefile
CROSS_COMPILE ?= aarch64-linux-gnu-
CC := $(CROSS_COMPILE)gcc

# 关键编译选项解释：
CFLAGS := \
    -ffreestanding \    # 不假设有 main()，不依赖标准库
    -nostdlib \         # 不链接 libc
    -nostdinc \         # 不搜索系统头文件
    -march=armv8-a \    # 目标: ARMv8-A 指令集
    -O1 -g              # 基础优化 + 调试信息

OBJS := arch/arm64/kernel/head.o \
        arch/arm64/kernel/entry.o \
        kernel/main.o \
        kernel/printk.o
```

**为什么用 `-ffreestanding`？** 因为我们没有操作系统，`main()` 不存在，标准库函数（`printf`, `malloc`）也不存在。编译器需要知道这一点，否则它可能生成调用 `memcpy` 等库函数的代码。

---

## 1.2 链接脚本 — 告诉代码放在哪里

```ld
OUTPUT_ARCH(aarch64)
ENTRY(_start)

SECTIONS {
    . = 0x40080000;          /* QEMU 把内核加载到这个物理地址 */

    .head.text : {
        _text = .;
        KEEP(*(.head.text))  /* ARM64 Image 头部，必须在最前 */
    }

    .text : { *(.text .text.*) }

    . = ALIGN(4096);
    .rodata : { *(.rodata .rodata.*) }

    . = ALIGN(4096);
    .data : { *(.data .data.*) }

    . = ALIGN(16);
    _bss_start = .;
    .bss (NOLOAD) : { *(.bss .bss.*) *(COMMON) }
    . = ALIGN(16);
    _bss_end = .;
    _end = .;
}
```

**原理**：

- `0x40080000` — QEMU virt 机器的 RAM 从 `0x40000000` 开始，内核加载到 `+0x80000` 偏移处
- `.head.text` — ARM64 Linux Image 头部（64 字节），QEMU 靠这个识别内核
- `.bss (NOLOAD)` — 未初始化全局变量，不占 ELF 文件体积，启动时清零
- `_bss_start` / `_bss_end` — head.S 用这两个符号清零 BSS

---

## 1.3 ARM64 Image 头部 — 让 QEMU 认识我们

QEMU 加载 ARM64 内核时需要看到一个标准的 Image 头部。这是一个 64 字节的结构：

```asm
    .section ".head.text", "ax"
    .global _head
_head:
    b       _start              /* 跳转到真正入口（第一条指令） */
    .quad   0x0                 /* Image load offset: 0 */
    .quad   _end - _head        /* 镜像大小 */
    .quad   0x0                 /* flags: little-endian, 4KB page */
    .quad   0x0                 /* reserved */
    .quad   0x0                 /* reserved */
    .quad   0x0                 /* reserved */
    .ascii  "ARM\x64"           /* 魔数 — QEMU 靠这个识别 */
    .long   0x0                 /* PE/COFF header offset (无) */
```

**为什么第一条是 `b _start`？** 因为 bootloader 可能直接跳到镜像头部执行，这条跳转确保不会执行后面的元数据。

---

## 1.4 `_start` — 内核真正的第一行代码

当 QEMU 启动 CPU 后，从 EL2（Hypervisor 模式）开始执行。我们的任务是：

1. 关中断
2. 保存启动参数
3. 从 EL2 降级到 EL1
4. 设置栈
5. 清零 BSS
6. 安装异常向量表
7. 跳到 C 代码

### Step 1: 关闭所有中断

```asm
_start:
    msr     daifset, #0xf       /* D=debug, A=SError, I=IRQ, F=FIQ 全部屏蔽 */
```

**原理**：`DAIF` 是 ARM64 的中断屏蔽位。在初始化完成前，任何中断都会导致崩溃（因为没有处理函数），所以先全部关掉。

### Step 2: 保存启动参数

```asm
    adr     x21, boot_args      /* PC 相对寻址，无 MMU 也能用 */
    str     x0,  [x21]          /* boot_args[0] = FDT 物理地址 */
    str     x1,  [x21, #8]
    str     x2,  [x21, #16]
    str     x3,  [x21, #24]
```

**原理**：ARM64 Boot Protocol 规定 x0 = FDT（设备树）地址，x1-x3 保留为 0。QEMU 会自动设置 x0。`adr` 是 PC 相对寻址指令，在 MMU 关闭时用它获取物理地址。

### Step 3: 检查异常级别

```asm
    mrs     x0, CurrentEL       /* 读取当前异常级别 */
    and     x0, x0, #0xc        /* bits[3:2] = EL */
    cmp     x0, #8              /* 0x8 = EL2 (2 << 2) */
    beq     from_el2
    cmp     x0, #4              /* 0x4 = EL1 */
    beq     setup_el1
    b       .                   /* EL0 或 EL3: 不支持，挂死 */
```

**原理**：`CurrentEL` 寄存器的 bits[3:2] 编码当前 EL。QEMU 通常从 EL2 启动，我们需要降到 EL1（内核模式）。

### Step 4: EL2 → EL1 降级

```asm
from_el2:
    mov     x0, #(1 << 31)      /* HCR_EL2.RW = 1: EL1 使用 AArch64 */
    msr     hcr_el2, x0

    msr     sctlr_el1, xzr      /* SCTLR_EL1 = 0: MMU 关, Cache 关 */
    isb                          /* 指令同步屏障：确保设置生效 */

    mov     x0, #0x3c5          /* SPSR_EL2: 返回到 EL1h, DAIF 全屏蔽 */
    msr     spsr_el2, x0

    adr     x0, setup_el1       /* ELR_EL2: eret 后跳转到 setup_el1 */
    msr     elr_el2, x0

    eret                         /* 执行！切换到 EL1 */
```

**原理**：

```
                        ┌─────────┐
  QEMU 启动 ─────────► │   EL2   │  Hypervisor 模式
                        └────┬────┘
                    eret     │   HCR_EL2.RW=1 → AArch64
                             ▼
                        ┌─────────┐
                        │   EL1   │  内核模式（我们要在这里运行）
                        └─────────┘
```

- `HCR_EL2.RW = 1`：告诉 CPU，EL1 使用 64 位模式
- `SPSR_EL2 = 0x3c5`：`eret` 后的处理器状态 — EL1h（使用 SP_EL1）+ 中断全屏蔽
- `ELR_EL2`：`eret` 后的跳转地址
- `eret`：特殊的返回指令，同时切换异常级别

### Step 5: 设置内核栈

```asm
setup_el1:
    adr     x0, init_stack_top   /* 栈顶地址 */
    mov     sp, x0
```

```asm
/* 数据区：8KB 初始栈 */
    .section ".data", "aw"
    .balign 16
    .skip   8192                 /* 8KB 栈空间 */
init_stack_top:                  /* 栈从此向下增长 */
```

**原理**：ARM64 栈向下增长。`sp` 指向栈顶（最高地址），每次 `push` 操作 `sp` 递减。AArch64 ABI 要求 16 字节对齐。

```
    低地址                              高地址
    ┌──────────────────────────────────┐
    │           8KB 栈空间              │
    │  ◄─── 栈向下增长                 │ ◄── init_stack_top (sp 初始值)
    └──────────────────────────────────┘
```

### Step 6: 清零 BSS

```asm
    adr     x0, _bss_start
    adr     x1, _bss_end
.Lbss_loop:
    cmp     x0, x1
    b.ge    .Lbss_done
    stp     xzr, xzr, [x0], #16    /* 一次清零 16 字节 */
    b       .Lbss_loop
.Lbss_done:
```

**原理**：C 标准规定未初始化的全局变量值为 0。`stp xzr, xzr` 同时写入两个零寄存器（16 字节），高效清零。

### Step 7: 安装异常向量表并跳转到 C

```asm
    adr     x0, vectors          /* vectors 定义在 entry.S */
    msr     vbar_el1, x0         /* VBAR_EL1 = 向量表基址 */
    isb                          /* 确保生效 */

    bl      start_kernel         /* 跳转到 C！ */
    b       .                    /* 不应返回 */
```

---

## 1.5 异常向量表 — 硬件的"中断跳转表"

### 向量表结构

ARM64 异常向量表有 16 个入口（4 种来源 × 4 种类型），每个 128 字节，总共 2KB：

```
            ┌───────────────┬────────────┬────────────┬────────────┐
            │   Synchronous │    IRQ     │    FIQ     │   SError   │
  ──────────┼───────────────┼────────────┼────────────┼────────────┤
  EL1 SP_EL0│ el1t_sync     │ el1t_irq   │ el1t_fiq   │ el1t_error │
  EL1 SP_EL1│ el1h_sync     │ el1h_irq   │ el1h_fiq   │ el1h_error │
  EL0 64-bit│ el0_sync      │ el0_irq    │ el0_fiq    │ el0_error  │
  EL0 32-bit│ el0_32_sync   │ el0_32_irq │ el0_32_fiq │ el0_32_error│
  ──────────┴───────────────┴────────────┴────────────┴────────────┘
```

### 实现

```asm
/* 向量入口宏：对齐到 128 字节并跳转 */
.macro  ventry  label
    .align  7                    /* 2^7 = 128 字节对齐 */
    b       \label
.endm

    .align  11                   /* 2^11 = 2KB 对齐（整个表） */
    .global vectors
vectors:
    /* Group 1: 当前 EL，SP_EL0（配置错误路径）*/
    ventry  el1t_sync
    ventry  el1t_irq
    ventry  el1t_fiq
    ventry  el1t_error

    /* Group 2: 当前 EL，SP_EL1（内核正常路径）*/
    ventry  el1h_sync            /* ← 内核同步异常走这里 */
    ventry  el1h_irq             /* ← 内核 IRQ 走这里（Phase 3） */
    ventry  el1h_fiq
    ventry  el1h_error

    /* Group 3: 来自 EL0（用户态），AArch64 */
    ventry  el0_sync             /* ← 用户态系统调用走这里（Phase 5） */
    ventry  el0_irq
    ventry  el0_fiq
    ventry  el0_error

    /* Group 4: 来自 EL0，AArch32（不支持）*/
    ventry  el0_32_sync
    ventry  el0_32_irq
    ventry  el0_32_fiq
    ventry  el0_32_error
```

### kernel_entry / kernel_exit 宏 — 保存和恢复现场

异常发生时，CPU 需要保存当前所有寄存器（"现场"），处理完后恢复。这就是 `pt_regs` 结构：

```asm
.macro  kernel_entry, el
    sub     sp, sp, #272         /* 在栈上分配 pt_regs (34 × 8 字节) */

    stp     x0,  x1,  [sp, #0]  /* 保存 x0-x1 */
    stp     x2,  x3,  [sp, #16] /* 保存 x2-x3 */
    ...                          /* x4-x29，每对 16 字节 */
    str     x30, [sp, #240]      /* 保存 LR */

    mrs     x21, elr_el1         /* 异常返回地址 */
    mrs     x22, spsr_el1        /* 处理器状态 */
    stp     x21, x22, [sp, #256] /* 保存 PC 和 PSTATE */
.endm
```

```
    pt_regs 内存布局（272 字节）:
    ┌─────────────┐ sp + 0
    │  x0  │  x1  │
    ├──────┼──────┤ sp + 16
    │  x2  │  x3  │
    ├──────┼──────┤
    │  ... │ ...  │
    ├──────┼──────┤ sp + 240
    │  LR (x30)   │
    ├──────────────┤ sp + 248
    │  SP (用户态) │
    ├──────────────┤ sp + 256
    │  PC (ELR)    │
    ├──────────────┤ sp + 264
    │  PSTATE      │
    └──────────────┘ sp + 272
```

### Phase 1 异常处理：打印信息并挂死

```asm
el1h_sync:
    kernel_entry 1
    mov     x0, sp               /* x0 = pt_regs 指针 */
    bl      handle_sync_exception /* 调用 C 函数 */
    kernel_exit 1                /* Phase 1 不会到这里（死循环） */
```

---

## 1.6 串口输出 — PL011 UART

QEMU virt 机器提供一个 PL011 UART，物理地址 `0x09000000`。QEMU 已经初始化好了，我们只需写字符：

```c
#define PL011_BASE  0x09000000UL
#define UARTDR      0x000       /* 数据寄存器 */
#define UARTFR      0x018       /* 标志寄存器 */
#define UARTFR_TXFF (1 << 5)   /* 发送 FIFO 满 */

#define PL011_REG(offset) \
    (*((volatile unsigned int *)(PL011_BASE + (offset))))

static void uart_putchar(char c)
{
    /* 轮询等待：FIFO 有空位才能写 */
    while (PL011_REG(UARTFR) & UARTFR_TXFF)
        ;
    PL011_REG(UARTDR) = (unsigned int)(unsigned char)c;
}

void boot_printk(const char *s)
{
    while (*s) {
        if (*s == '\n')
            uart_putchar('\r');  /* 串口需要 CR+LF */
        uart_putchar(*s++);
    }
}
```

**原理**：MMIO（内存映射 I/O）— 硬件寄存器映射到固定物理地址，用指针读写就是在操作硬件。`volatile` 告诉编译器不要优化掉这些读写。

---

## 1.7 `start_kernel()` — 第一个 C 函数

```c
void start_kernel(void)
{
    boot_printk("[BOOT] ARM64 kernel starting...\n");

    /* 打印内核镜像布局 */
    boot_printk("[BOOT] Kernel text   : ");
    boot_printk_hex((unsigned long)_text);    /* 链接脚本定义 */
    boot_printk("\n");

    boot_printk("[BOOT] Kernel end    : ");
    boot_printk_hex((unsigned long)_end);
    boot_printk("\n");

    /* 验证异常向量表已安装 */
    unsigned long vbar;
    __asm__ volatile("mrs %0, vbar_el1" : "=r"(vbar));
    boot_printk("[BOOT] VBAR_EL1      : ");
    boot_printk_hex(vbar);
    boot_printk("\n");

    while (1)
        ;   /* Phase 1 结束，挂死 */
}
```

---

## 1.8 完整启动流程图

```
  QEMU 上电 (EL2, MMU 关, 物理地址 0x40080000)
        │
        ▼
  ┌─────────────────┐
  │   _head (Image头)│ ── b _start
  └────────┬────────┘
           ▼
  ┌─────────────────┐
  │   _start         │
  │  1. msr daifset  │  关中断
  │  2. str x0-x3    │  保存启动参数
  │  3. mrs CurrentEL│  检查 EL
  └────────┬────────┘
           ▼
  ┌─────────────────┐
  │  from_el2        │
  │  HCR_EL2.RW = 1 │  设置 AArch64
  │  SPSR = 0x3c5    │  目标: EL1h
  │  ELR = setup_el1 │
  │  eret             │  ── 切换到 EL1 ──►
  └────────┬────────┘
           ▼
  ┌─────────────────┐
  │  setup_el1       │  (现在在 EL1)
  │  4. mov sp       │  设置 8KB 栈
  │  5. stp xzr 循环 │  清零 BSS
  │  6. msr vbar_el1 │  安装异常向量表
  │  7. bl start_kernel
  └────────┬────────┘
           ▼
  ┌─────────────────┐
  │  start_kernel()  │  ← 第一行 C 代码！
  │  boot_printk()   │  串口输出
  │  while(1);       │  Phase 1 完成
  └─────────────────┘
```

---

## 1.9 运行验证

```bash
make                # 编译
make run            # QEMU 中运行
```

期望输出：

```
[BOOT] ARM64 kernel starting...
[BOOT] Kernel text   : 0x0000000040080000
[BOOT] Kernel end    : 0x00000000400xxxxx
[BOOT] VBAR_EL1      : 0x00000000400xxxxx
```

---

## 1.10 Phase 1 核心概念总结

| 概念 | 说明 |
|------|------|
| **异常级别 (EL)** | EL3(安全) > EL2(虚拟化) > EL1(内核) > EL0(用户) |
| **ERET** | 从高 EL 返回低 EL，同时切换处理器状态 |
| **VBAR_EL1** | 异常向量表基址寄存器，CPU 异常时自动跳转 |
| **DAIF** | Debug/SError/IRQ/FIQ 中断屏蔽位 |
| **MMIO** | 内存映射 I/O，通过指针读写硬件寄存器 |
| **pt_regs** | 异常现场（34 个 64 位寄存器的快照） |
| **BSS** | 未初始化全局变量段，启动时清零 |

**Phase 1 奠定的基础**：CPU 在 EL1 运行，有栈可以调用 C 函数，有异常向量表可以捕获异常，有串口可以输出调试信息。接下来 Phase 2 将开启 MMU。
