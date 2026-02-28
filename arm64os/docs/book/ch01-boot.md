# Phase 1：启动与异常向量

## 知识来源总览

Phase 1 的代码来源分布：

- **ARM64 启动协议**：约 30%（Image header 格式、EL2 到 EL1 转换）
- **ARMv8-A 系统寄存器手册**：约 25%（SCTLR_EL1、HCR_EL2、SPSR_EL2、VBAR_EL1）
- **QEMU virt 平台规范**：约 15%（UART 地址 0x09000000、RAM 起始 0x40000000）
- **GNU 链接器脚本语法**：约 10%（ENTRY、SECTIONS、ALIGN）
- **Phase 1 文档本身**：约 20%（文件组织、测试步骤）

## 链接器脚本 linker.ld

### 入口地址

```
ENTRY(_start)
```

**来源：GNU ld 语法**。告诉链接器 ELF 入口点是 `_start` 符号。QEMU 的 `-kernel` 参数加载 ELF 后跳转到此地址。

### 物理地址

```
. = 0x40080000;
```

**来源：QEMU virt 平台 + ARM64 Image 协议**。

QEMU virt 机器的 RAM 从 0x40000000 开始（这是 QEMU 源码 `hw/arm/virt.c` 中硬编码的 `memmap[VIRT_MEM].base`）。

偏移 0x80000（512KB）的原因：ARM64 Linux Image 协议要求内核加载到 RAM 起始 + 2MB 对齐的偏移处。QEMU 的 `-kernel` 默认将镜像加载到 RAM+0x80000。如果设为 0x40000000，QEMU 仍然会把镜像放在 0x40080000，导致链接地址与加载地址不匹配，所有绝对地址引用都会出错。

### 段布局

```
.text : { *(.text .text.*) }
.rodata : { *(.rodata .rodata.*) }
.data : { *(.data .data.*) }
```

**来源：ELF 标准段名约定**。`.text` = 代码，`.rodata` = 只读数据，`.data` = 可读写数据。通配符 `*(.text .text.*)` 匹配所有输入文件的 `.text` 和 `.text.xxx` 段（如 `.text.unlikely`）。

### BSS 段对齐

```
. = ALIGN(16);
_bss_start = .;
.bss : { *(.bss .bss.*) *(COMMON) }
_bss_end = .;
```

**来源：ARM64 ABI 要求栈 16 字节对齐**。BSS 段存放未初始化全局变量。`_bss_start` 和 `_bss_end` 符号在 `head.S` 中用于清零 BSS。`COMMON` 段捕获未显式分配的全局符号（C 语言的 tentative definition）。

## head.S 启动入口

### ARM64 Image Header

```asm
    b _start           /* 跳转到入口点（偏移 0）*/
    .quad 0            /* Image加载偏移 */
    .quad _end - _start /* 镜像大小 */
    .quad 0            /* 标志 */
    .quad 0            /* 保留 */
    .quad 0            /* 保留 */
    .quad 0            /* 保留 */
    .quad 0            /* 保留 */
    .ascii "ARM\x64"   /* 魔数 */
    .long 0            /* PE header偏移（UEFI） */
```

**来源：Linux 内核文档 `Documentation/arm64/booting.rst`**。

ARM64 Image 必须以 64 字节的标准头部开始。引导加载器（QEMU、U-Boot、UEFI）通过检查偏移 56 处的魔数 `ARM\x64` 确认这是一个合法的 ARM64 内核镜像。

第一条指令 `b _start` 的位置恰好是偏移 0——引导加载器跳转到镜像起始地址时，直接执行这条分支指令，跳过剩余的头部数据到真正的入口代码。

### EL2 到 EL1 转换

```asm
_start:
    mrs x0, CurrentEL
    lsr x0, x0, #2
    cmp x0, #2
    b.ne 1f

    /* 配置 EL2 → EL1 */
    msr sctlr_el1, xzr        /* 清除 SCTLR_EL1 */
    mov x0, #(1 << 31)
    msr hcr_el2, x0           /* HCR_EL2.RW=1 → EL1 使用 AArch64 */
    mov x0, #0x3c5
    msr spsr_el2, x0          /* SPSR: EL1h, DAIF 全屏蔽 */
    adr x0, 1f
    msr elr_el2, x0           /* 返回地址 */
    eret                       /* 执行异常返回 → 进入 EL1 */
1:
```

**来源：ARMv8-A Architecture Reference Manual, Chapter D1**。

**`mrs x0, CurrentEL`**：读取当前异常级别。CurrentEL 寄存器的 bit[3:2] 编码 EL 值：`00`=EL0, `01`=EL1, `10`=EL2, `11`=EL3。QEMU virt 默认从 EL2 启动。

**`msr sctlr_el1, xzr`**：将 SCTLR_EL1（System Control Register）清零。关闭 MMU（bit 0 = M）、关闭数据缓存（bit 2 = C）、关闭指令缓存（bit 12 = I）。确保进入 EL1 时是一个已知的干净状态。

**`mov x0, #(1 << 31)`** 然后 **`msr hcr_el2, x0`**：设置 HCR_EL2（Hypervisor Configuration Register）的 RW 位（bit 31）。RW=1 表示 EL1 使用 AArch64 模式（而非 AArch32）。如果不设这一位，EL1 会以 32 位模式运行。

**`mov x0, #0x3c5`** 然后 **`msr spsr_el2, x0`**：SPSR_EL2（Saved Program Status Register）决定 `eret` 后的处理器状态。

0x3c5 的二进制分解：
```
bit [3:0] = 0101 = EL1h（使用 EL1 的专用栈指针 SP_EL1）
bit [6]   = 1    = F（FIQ 屏蔽）
bit [7]   = 1    = I（IRQ 屏蔽）
bit [8]   = 1    = A（SError 屏蔽）
bit [9]   = 1    = D（Debug 屏蔽）
```

屏蔽所有中断是因为此时中断控制器（GIC）尚未初始化，如果收到中断会跳转到未设置的向量表导致崩溃。

**`eret`**：异常返回指令。处理器从 EL2 切换到 SPSR_EL2 指定的级别（EL1h），PC 设为 ELR_EL2 的值（标号 `1:`），PSTATE 设为 SPSR_EL2 的值（DAIF 屏蔽）。

### 栈设置与 BSS 清零

```asm
1:
    ldr x0, =_stack_top
    mov sp, x0

    ldr x0, =_bss_start
    ldr x1, =_bss_end
2:  cmp x0, x1
    b.ge 3f
    str xzr, [x0], #8
    b 2b
3:
```

**栈设置**：`_stack_top` 在链接器脚本中定义，通常是 BSS 段末尾 + 一段预留空间（如 16KB）。ARM64 栈向下增长，SP 初始化为栈顶地址。

**BSS 清零**：C 语言规范要求未初始化的全局变量为零。但 ELF 文件中 BSS 段不占空间（只记录大小），加载后内存中的值是随机的。必须在调用任何 C 代码之前将 BSS 区域清零。

`str xzr, [x0], #8`：将零写入 x0 指向的地址，然后 x0 += 8（后索引寻址）。每次清零 8 字节（64 位）。

### 向量表安装

```asm
    ldr x0, =vectors
    msr vbar_el1, x0
    isb
```

**来源：ARMv8-A ARM, D1.10 Exception vector table**。

`vbar_el1`（Vector Base Address Register）指向异常向量表的基地址。向量表必须 2048 字节（0x800）对齐。ARM64 的向量表有 16 个条目，每个 128 字节（32 条指令的空间）。

`isb`（Instruction Synchronization Barrier）：确保 vbar_el1 的写入在后续指令获取前生效。因为处理器可能已经预取了后续指令，不知道向量表地址变了。

### 跳转到 C 代码

```asm
    bl start_kernel
    b .
```

`bl start_kernel`：Branch with Link，跳转到 C 函数 `start_kernel()`，同时将返回地址存入 LR（x30）。`start_kernel` 不应该返回。

`b .`：无限循环。安全网——如果 `start_kernel` 意外返回，处理器原地转圈而不是执行随机内存中的数据。

## entry.S 异常向量表

### 向量表布局

```asm
.balign 0x800
vectors:
    /* 当前EL，使用SP_EL0 */
    .balign 0x80; b sync_el1t
    .balign 0x80; b irq_el1t
    .balign 0x80; b fiq_el1t
    .balign 0x80; b error_el1t

    /* 当前EL，使用SP_ELx */
    .balign 0x80; b sync_el1h
    .balign 0x80; b irq_el1h
    .balign 0x80; b fiq_el1h
    .balign 0x80; b error_el1h

    /* 低EL，AArch64 */
    .balign 0x80; b sync_el0_64
    .balign 0x80; b irq_el0_64
    .balign 0x80; b fiq_el0_64
    .balign 0x80; b error_el0_64

    /* 低EL，AArch32 */
    .balign 0x80; b sync_el0_32
    .balign 0x80; b irq_el0_32
    .balign 0x80; b fiq_el0_32
    .balign 0x80; b error_el0_32
```

**来源：ARMv8-A ARM, Table D1-7**。

ARM64 的向量表是一个 16 条目的数组，按两个维度组织：

**维度一（4 组）——异常来源**：
- 当前 EL，SP_EL0：内核使用用户态栈指针时（几乎不用）
- 当前 EL，SP_ELx：内核正常运行时（这是最常用的内核异常入口）
- 低 EL，AArch64：从用户态（EL0 64 位）进入
- 低 EL，AArch32：从用户态（EL0 32 位）进入

**维度二（4 种）——异常类型**：
- Synchronous：同步异常（SVC 系统调用、页错误、未定义指令）
- IRQ：外部中断
- FIQ：快速中断
- SError：系统错误（异步，如 ECC 内存错误）

`.balign 0x80`：每个条目对齐到 128 字节边界（0x80 = 128）。128 字节 = 32 条 ARM64 指令的空间——对于简单的处理器够用（保存几个寄存器 + 跳转到 C handler）。

### kernel_entry / kernel_exit 宏

```asm
.macro kernel_entry
    sub sp, sp, #272         /* pt_regs 大小 */
    stp x0, x1, [sp, #0]
    stp x2, x3, [sp, #16]
    /* ... 保存 x0-x30 */
    mrs x0, elr_el1
    mrs x1, spsr_el1
    stp x0, x1, [sp, #256]  /* 保存 ELR 和 SPSR */
.endm
```

**272 字节 = 34 个 8 字节寄存器**：x0-x30（31 个通用寄存器）+ SP + ELR_EL1 + SPSR_EL1。这就是 `struct pt_regs` 的内存布局。

`stp`（Store Pair）每次保存两个寄存器，比两次 `str` 更高效（一次总线事务写 16 字节）。

为什么保存 ELR 和 SPSR？`ELR_EL1` 是异常返回地址（被中断的代码的 PC），`SPSR_EL1` 是被中断时的处理器状态（包括条件标志和中断屏蔽位）。异常返回时需要恢复它们。

## printk.c PL011 UART 输出

```c
#define UART_BASE   0x09000000UL
#define UART_DR     (*(volatile unsigned int *)(UART_BASE + 0x000))
#define UART_FR     (*(volatile unsigned int *)(UART_BASE + 0x018))
#define UART_FR_TXFF (1 << 5)

void uart_putc(char c) {
    while (UART_FR & UART_FR_TXFF)
        ;
    UART_DR = c;
}
```

**来源：ARM PL011 Technical Reference Manual**。

`0x09000000`：QEMU virt 平台的 UART 基地址（`hw/arm/virt.c` 中的 `memmap[VIRT_UART].base`）。

`UART_DR`（Data Register，偏移 0x000）：写入一个字节触发串口发送。

`UART_FR`（Flag Register，偏移 0x018）：bit 5 = TXFF（Transmit FIFO Full）。写入前必须轮询等待 FIFO 非满。

`volatile`：告诉编译器每次访问都要真正读写内存，不能缓存到寄存器中——因为硬件寄存器的值可能随时被硬件改变。

## main.c 入口

```c
void start_kernel(void) {
    boot_printk("Booting arm64os...\n");
    boot_printk("Exception level: EL1\n");

    /* 触发一个同步异常（测试向量表）*/
    asm volatile("svc #0");

    boot_printk("Back from exception!\n");
}
```

`svc #0`：Supervisor Call，触发同步异常。处理器跳转到向量表的 sync_el1h 入口（因为当前在 EL1 使用 SP_EL1），执行异常处理程序后返回。这验证了向量表正确安装。

## Makefile 构建系统

```makefile
CROSS = aarch64-linux-gnu-
CC    = $(CROSS)gcc
LD    = $(CROSS)ld
OBJCOPY = $(CROSS)objcopy

CFLAGS = -ffreestanding -nostdlib -nostartfiles -Wall -O2
```

`-ffreestanding`：告诉 GCC 这是独立环境（没有标准库、没有 `main` 作为入口）。允许 GCC 不假定标准库函数的存在。

`-nostdlib`：不链接标准 C 库（glibc/musl）。我们的内核提供自己的运行时。

`-nostartfiles`：不链接 `crt0.o`（C 运行时启动文件）。我们用自己的 `head.S` 作为入口。
