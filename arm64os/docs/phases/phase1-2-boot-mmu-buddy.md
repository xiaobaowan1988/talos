# Phase 1 & 2：ARM64启动 + MMU + Buddy内存分配器

## 参考内核文件

```
arch/arm64/kernel/head.S          # 内核启动入口（必读）
arch/arm64/kernel/entry.S         # 异常向量表（必读）
arch/arm64/kernel/proc.S          # CPU初始化、TLB/Cache操作
arch/arm64/mm/mmu.c               # MMU初始化、页表建立
arch/arm64/mm/proc.S              # __cpu_setup, __enable_mmu
arch/arm64/include/asm/pgtable.h  # 页表项格式定义
arch/arm64/include/asm/memory.h   # 虚拟地址空间布局
mm/page_alloc.c                   # Buddy分配器核心
mm/memblock.c                     # 早期物理内存分配器
```

---

## Phase 1：ARM64启动与异常向量

### 1.1 启动流程（参考 head.S）

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

### 1.2 链接脚本（linker.ld）

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

### 1.3 head.S 实现要点

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

    /* 4. 建立页表、开启MMU → 见 1.4 节 */
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

### 1.4 异常向量表（参考 entry.S）

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

### 1.5 关键系统寄存器（参考 sysreg.h）

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

---

## Phase 2：MMU建立 + 4级页表 + Buddy分配器

### 2.1 ARMv8 4级页表结构（4KB页，48位VA）

```
虚拟地址（48位）解析：
 ┌─────────┬────────┬────────┬────────┬────────┬────────┐
 │ [63:48] │[47:39] │[38:30] │[29:21] │[20:12] │[11:0]  │
 │ 符号扩展 │ PGD索引│ PUD索引│ PMD索引│ PTE索引│ 页内偏移│
 │ (16位)  │  (9位) │  (9位) │  (9位) │  (9位) │ (12位) │
 └─────────┴────────┴────────┴────────┴────────┴────────┘

页表层级：
 PGD (L0) → 512项 × 512GB = 256TB（覆盖整个地址空间）
 PUD (L1) → 512项 × 1GB
 PMD (L2) → 512项 × 2MB（可直接映射2MB大页）
 PTE (L3) → 512项 × 4KB
```

```c
/* 参考 arch/arm64/include/asm/pgtable-hwdef.h */
/* 页表项格式（ARMv8 Block/Page descriptor）*/

/* Level 3 (PTE) Page descriptor */
#define PTE_VALID       (1UL << 0)   /* bit 0: 必须为1 */
#define PTE_PAGE        (1UL << 1)   /* bit 1: 1=页描述符 */
#define PTE_AF          (1UL << 10)  /* Access Flag: 首次访问置1 */
#define PTE_nG          (1UL << 11)  /* not Global: ASID区分 */
#define PTE_SH_INNER    (3UL << 8)   /* Inner Shareable */
#define PTE_AP_RW_EL1   (0UL << 6)   /* 内核读写，用户无权 */
#define PTE_AP_RW_ALL   (1UL << 6)   /* 内核/用户读写 */
#define PTE_AP_RO_EL1   (2UL << 6)   /* 内核只读 */
#define PTE_UXN         (1UL << 54)  /* 用户态不可执行 */
#define PTE_PXN         (1UL << 53)  /* 特权态不可执行 */

/* 内存属性索引（MAIR_EL1）*/
#define MT_NORMAL       0   /* 普通内存（可缓存）*/
#define MT_NORMAL_NC    1   /* 普通内存（不可缓存）*/
#define MT_DEVICE_nGnRnE 2  /* 设备内存（MMIO）*/

/* MAIR_EL1 配置 */
#define MAIR_EL1_VALUE  \
    (0xFFUL << (8 * MT_NORMAL))    | \  /* 普通内存 */
    (0x44UL << (8 * MT_NORMAL_NC)) | \  /* 普通内存NC */
    (0x00UL << (8 * MT_DEVICE_nGnRnE))  /* 设备内存 */
```

### 2.2 早期页表建立（参考 mmu.c __create_page_tables）

```c
/*
 * 策略：早期只建立最小映射（跑起来后再建完整映射）
 * 1. Identity mapping: 物理地址 == 虚拟地址（让MMU开启那一刻不崩）
 * 2. Kernel mapping:   将内核映射到高虚拟地址（TTBR1）
 */

/* 内存布局常量（参考 arch/arm64/include/asm/memory.h）*/
#define VA_BITS         48
#define PAGE_SHIFT      12
#define PAGE_SIZE       (1 << PAGE_SHIFT)       /* 4KB */
#define PGDIR_SHIFT     39                       /* PGD索引起始位 */
#define PUD_SHIFT       30
#define PMD_SHIFT       21
#define PTRS_PER_PGD    512

/* 内核虚拟地址空间起始 */
#define KIMAGE_VADDR    0xFFFF000000000000UL
/* 线性映射（物理内存直接映射）起始 */
#define PAGE_OFFSET     0xFFFF800000000000UL
/* 物理内存起始（QEMU virt machine）*/
#define PHYS_OFFSET     0x40000000UL
```

### 2.3 Buddy分配器设计（参考 mm/page_alloc.c）

Buddy系统是Linux物理内存管理的核心。理解它需要掌握：

```
核心数据结构：

struct page {
    unsigned long flags;     /* 页标志 */
    union {
        struct {
            struct list_head lru;    /* Buddy链表 */
        };
        unsigned long _mapcount;
    };
    atomic_t _refcount;
    unsigned int order;      /* 当前页块的order（仅首页有效）*/
};

struct free_area {
    struct list_head free_list; /* 空闲页链表 */
    unsigned long    nr_free;   /* 空闲页块数量 */
};

struct zone {
    struct free_area free_area[MAX_ORDER]; /* order 0..10 */
    /* MAX_ORDER = 11，最大连续分配 2^10 = 1024页 = 4MB */
};
```

```c
/* Buddy算法核心操作 */

/* 分配：从free_area[order]取一块，若没有则向上找更大的块并split */
static struct page *
__rmqueue_smallest(struct zone *zone, unsigned int order) {
    struct free_area *area;
    struct page *page;

    for (; order < MAX_ORDER; ++order) {
        area = &zone->free_area[order];
        if (list_empty(&area->free_list))
            continue;

        page = list_first_entry(&area->free_list, struct page, lru);
        list_del(&page->lru);
        area->nr_free--;

        /* 如果找到的块比需要的大，把多余的部分放回（expand）*/
        expand(zone, page, order, /* 目标order */, area);
        return page;
    }
    return NULL;
}

/* 释放：将页归还，尝试与buddy合并（merge up）*/
static void __free_one_page(struct page *page, unsigned long pfn,
                            struct zone *zone, unsigned int order) {
    while (order < MAX_ORDER - 1) {
        /* 找到buddy的PFN */
        unsigned long buddy_pfn = pfn ^ (1 << order);
        struct page *buddy = pfn_to_page(buddy_pfn);

        /* 检查buddy是否也是空闲的同order块 */
        if (!page_is_buddy(page, buddy, order))
            break;

        /* 合并：将buddy从链表摘除，order提升 */
        list_del(&buddy->lru);
        zone->free_area[order].nr_free--;

        /* 使用较低PFN作为合并后块的首页 */
        if (buddy_pfn < pfn) {
            page = buddy;
            pfn = buddy_pfn;
        }
        order++;
    }
    /* 将合并后的块加入对应order的free_list */
    list_add(&page->lru, &zone->free_area[order].free_list);
    zone->free_area[order].nr_free++;
}
```

### 2.4 实现里程碑 & 验证方法

**Phase 1 验证（QEMU串口输出）：**
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
# [BOOT] MMU enabled, running at VA 0xFFFF000040080000
# [BOOT] BSS cleared: 0xFFFF000040100000 - 0xFFFF000040200000
# start_kernel() reached
```

**Phase 2 验证：**
```c
// 在 start_kernel() 中验证：
void test_buddy(void) {
    // 分配 order=0（4KB）
    struct page *p1 = alloc_pages(GFP_KERNEL, 0);
    // 分配 order=2（16KB）
    struct page *p2 = alloc_pages(GFP_KERNEL, 2);
    // 释放并验证合并
    free_pages(p2, 2);
    // p2+p1的buddy应能合并为更大块
    free_pages(p1, 0);

    printk("Buddy allocator: OK\n");
}
```

### 2.5 关键难点与注意事项

1. **identity map窗口**：MMU开启瞬间CPU还在物理地址执行，必须保证物理地址和虚拟地址都能访问到同一条指令（`isb`之前用identity map，之后跳转到KIMAGE_VADDR高地址）。

2. **KASLR简化**：教学实现可以固定加载地址，不实现随机化。

3. **TLB shootdown**：单核不需要，多核时修改页表后必须广播TLB无效化（`dsb ishst` + `tlbi vmalle1is` + `dsb ish` + `isb`）。

4. **memblock vs buddy**：MMU开启前用`memblock`分配早期内存（简单线性分配），buddy初始化后才能动态管理。

5. **struct page内存**：1GB物理内存需要约4MB的`struct page`数组（每个4KB页一个struct page，每个struct page约64字节，1GB/4KB * 64 = 16MB）。

### 2.6 本阶段产出文件

```
arm64os/
├── arch/arm64/kernel/head.S      ← Phase 1 核心
├── arch/arm64/kernel/entry.S     ← Phase 1 核心
├── arch/arm64/mm/mmu.c           ← Phase 2 核心
├── arch/arm64/mm/tlb.S           ← Phase 2 辅助
├── arch/arm64/include/asm/
│   ├── pgtable.h
│   ├── memory.h
│   └── sysreg.h
├── mm/
│   ├── memblock.c                ← Phase 2 早期分配
│   └── page_alloc.c              ← Phase 2 核心
├── kernel/printk.c               ← 串口调试输出
├── scripts/linker.ld
└── Makefile
```
