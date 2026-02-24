# Phase 2 Walkthrough: MMU + 页表 + Buddy 分配器 — 从物理地址到虚拟内存

> **目标**：开启 MMU，建立内存管理，实现物理页分配器。
> **最终效果**：`alloc_pages(order)` 和 `free_pages()` 正常工作，Buddy 合并验证通过。

---

## 2.1 为什么需要 MMU？

Phase 1 直接用物理地址运行。这有几个致命问题：

1. **没有内存保护** — 任何代码都能读写任何地址
2. **不能运行用户程序** — 用户程序和内核共享地址空间
3. **不能用虚拟地址** — 无法实现进程隔离

MMU（Memory Management Unit）在 CPU 和物理内存之间插入一层**地址翻译**：

```
  CPU 发出虚拟地址 (VA)
        │
        ▼
  ┌───────────┐
  │    MMU    │ ── 查页表 ──
  └─────┬─────┘
        ▼
  物理地址 (PA) → 访问内存
```

---

## 2.2 ARMv8 四级页表结构

ARM64 使用 48 位虚拟地址，通过 4 级页表翻译：

```
  虚拟地址 (48 位):
  ┌──────┬──────┬──────┬──────┬────────────┐
  │ L0   │ L1   │ L2   │ L3   │ Page Offset│
  │[47:39]│[38:30]│[29:21]│[20:12]│  [11:0]  │
  │ 9 bit│ 9 bit│ 9 bit│ 9 bit│  12 bit    │
  └──┬───┴──┬───┴──┬───┴──┬───┴─────┬──────┘
     │      │      │      │         │
     ▼      ▼      ▼      ▼         ▼
   PGD    PUD    PMD    PTE     4KB 页内偏移
  512项   512项   512项   512项
```

每级 9 位索引 → 512 个条目 → 每个条目 8 字节 → 每级页表 4KB（正好一页）。

**关键寻址范围**：
- L0 (PGD) 一个条目覆盖 512GB
- L1 (PUD) 一个条目覆盖 1GB — **Phase 2 用 L1 块映射**
- L2 (PMD) 一个条目覆盖 2MB
- L3 (PTE) 一个条目覆盖 4KB

---

## 2.3 Phase 2 的简化策略 — 恒等映射

Phase 2 使用**恒等映射**（Identity Mapping）：虚拟地址 = 物理地址。

```
  VA 0x40080000 ──MMU──► PA 0x40080000 （内核代码）
  VA 0x09000000 ──MMU──► PA 0x09000000 （UART 设备）
  VA 0x08000000 ──MMU──► PA 0x08000000 （GIC 中断控制器）
```

**为什么不直接用高虚拟地址？** 因为 Phase 2 的重点是理解 MMU 机制，恒等映射最简单，且代码无需修改（地址不变）。

### 页表数据结构

```c
/* 静态分配，4KB 对齐 */
static unsigned long init_pgd[512] __attribute__((aligned(4096)));
static unsigned long init_pud[512] __attribute__((aligned(4096)));
```

只需两级：PGD (L0) → PUD (L1)，用 L1 块描述符映射 1GB 大块。

---

## 2.4 建立页表 — `create_page_tables()`

```c
static void create_page_tables(void)
{
    int i;

    /* 清零页表 */
    for (i = 0; i < 512; i++) {
        init_pgd[i] = 0;
        init_pud[i] = 0;
    }

    /* PGD[0] → PUD 表（Table 描述符） */
    init_pgd[0] = (unsigned long)init_pud | PD_TABLE;

    /* PUD[0]: 0x00000000 - 0x3FFFFFFF → 设备内存 */
    init_pud[0] = (0x00000000UL)
                | PD_BLOCK           /* L1 块描述符（1GB）*/
                | PTE_AF             /* Access Flag */
                | PTE_ATTR(MT_DEVICE_nGnRnE)  /* 设备内存属性 */
                | PTE_PXN | PTE_UXN; /* 不可执行 */

    /* PUD[1]: 0x40000000 - 0x7FFFFFFF → 普通 RAM */
    init_pud[1] = (0x40000000UL)
                | PD_BLOCK
                | PTE_AF
                | PTE_ATTR(MT_NORMAL) /* 普通内存（可缓存）*/
                | PTE_SH_INNER;       /* 内部共享 */
}
```

**映射布局**：

```
  虚拟地址空间 (只用 L0[0] → L1)
  ┌───────────────────────────────────┐ 0x00000000
  │   L1[0]: 设备内存 (1GB)           │ UART, GIC, VirtIO MMIO
  │   属性: MT_DEVICE_nGnRnE          │ 不缓存，严格顺序
  ├───────────────────────────────────┤ 0x40000000
  │   L1[1]: RAM (1GB)                │ 内核代码+数据+堆
  │   属性: MT_NORMAL                  │ 可缓存，write-back
  └───────────────────────────────────┘ 0x80000000
```

**页表条目格式**：

```
  L1 Block 描述符 (64 位):
  ┌──────────────────────────────────────────────────────────┐
  │ Upper Attrs│  物理地址[47:30]  │ Lower Attrs │ Type │ V │
  │  [63:52]   │    [47:30]        │  [11:2]     │ [1]  │[0]│
  └──────────────────────────────────────────────────────────┘
      PXN/UXN     输出物理地址       AF/SH/AP/    0=Block 1=Valid
                   (1GB 对齐)        AttrIdx
```

---

## 2.5 CPU 配置 — MAIR + TCR

开启 MMU 前，必须配置两个关键系统寄存器。这在 `proc.S` 中用汇编实现：

### MAIR_EL1 — 内存属性索引

```asm
cpu_init:
    /* MAIR_EL1: 定义 4 种内存类型 */
    ldr     x0, =0xFF44040000000000  /* 简化：只设置低 32 位 */
    msr     mair_el1, x0
```

实际设置的值：

| Index | 属性 | 用途 |
|-------|------|------|
| 0 | `0x00` Device-nGnRnE | MMIO 寄存器（最严格，不缓存不重排） |
| 1 | `0x04` Device-nGnRE | MMIO（允许聚合写） |
| 2 | `0x44` Normal-NC | 普通内存不缓存（DMA 用） |
| 3 | `0xFF` Normal-WBWA | 普通内存（读写分配，write-back） |

### TCR_EL1 — 翻译控制

```asm
    /* TCR_EL1: 配置地址翻译参数 */
    ldr     x0, =(TCR_T0SZ_48   | \   /* 48 位虚拟地址空间 */
                   TCR_IRGN0_WB  | \   /* 页表 walk 缓存策略 */
                   TCR_ORGN0_WB  | \
                   TCR_SH0_INNER | \   /* 页表共享属性 */
                   TCR_TG0_4K    | \   /* 4KB 页粒度 */
                   TCR_IPS_48)         /* 48 位物理地址 */
    msr     tcr_el1, x0
```

---

## 2.6 开启 MMU — `enable_mmu()`

```asm
enable_mmu:
    /* 写入页表基址寄存器 */
    msr     ttbr0_el1, x0        /* TTBR0: 用户/恒等映射 */
    mov     x1, xzr
    msr     ttbr1_el1, x1        /* TTBR1: 内核高地址（Phase 2 不用）*/

    dsb     ish                  /* 数据同步屏障 */
    isb                          /* 指令同步屏障 */

    /* 开启 MMU + D-cache + I-cache */
    mrs     x0, sctlr_el1
    orr     x0, x0, #1          /* M = 1: MMU 开 */
    orr     x0, x0, #(1 << 2)   /* C = 1: D-cache 开 */
    orr     x0, x0, #(1 << 12)  /* I = 1: I-cache 开 */
    msr     sctlr_el1, x0

    isb                          /* 确保 MMU 立即生效 */

    /* 刷新整个 TLB */
    tlbi    vmalle1
    dsb     ish
    isb

    ret
```

**开启过程的危险时刻**：设置 `SCTLR_EL1.M = 1` 的那一刻起，CPU 的每次内存访问都要经过 MMU 翻译。如果页表有错，CPU 立刻崩溃。**恒等映射**保证了切换瞬间地址不变，避免了这个问题。

---

## 2.7 Memblock — 早期物理内存分配器

MMU 开启后，我们需要管理物理内存。但 Buddy 分配器本身也需要分配内存来存储元数据。解决方案：先用简单的 **memblock** 分配器做引导。

### 数据结构

```c
struct memblock_region {
    phys_addr_t base;
    phys_addr_t size;
};

struct memblock_type {
    unsigned int cnt;
    struct memblock_region regions[16];
};

static struct memblock_type memblock_memory;    /* 可用内存 */
static struct memblock_type memblock_reserved;  /* 已保留内存 */
```

### 初始化

```c
void memblock_init(phys_addr_t phys_start, phys_addr_t phys_size)
{
    /* 注册整个 RAM 区域 */
    memblock_add(phys_start, phys_size);
    /* 0x40000000 - 0x80000000 (1GB) */

    /* 保留内核镜像 */
    memblock_reserve((phys_addr_t)_text,
                     (phys_addr_t)_end - (phys_addr_t)_text);

    /* 保留页表 */
    memblock_reserve((phys_addr_t)init_pgd, PAGE_SIZE);
    memblock_reserve((phys_addr_t)init_pud, PAGE_SIZE);
}
```

### 分配算法（自底向上）

```c
phys_addr_t memblock_alloc(phys_addr_t size, phys_addr_t align)
{
    /* 遍历可用内存区域 */
    for (i = 0; i < memblock_memory.cnt; i++) {
        candidate = ALIGN(region->base, align);

        /* 检查是否与已保留区域冲突 */
        for (j = 0; j < memblock_reserved.cnt; j++) {
            if (overlap(candidate, size, reserved[j]))
                candidate = reserved[j].base + reserved[j].size;
        }

        if (candidate + size <= region->base + region->size) {
            memblock_reserve(candidate, size);  /* 标记已用 */
            return candidate;
        }
    }
    return 0;  /* 失败 */
}
```

---

## 2.8 Buddy 分配器 — 物理页的"配对系统"

### 核心思想

Buddy 系统将物理内存分成 2^order 大小的块（order 0 = 4KB，order 10 = 4MB）。每个块有一个"伙伴"（buddy），地址通过翻转特定位计算：

```
  Order 0 (4KB): PFN 0 的伙伴是 PFN 1
  Order 1 (8KB): PFN 0-1 的伙伴是 PFN 2-3
  Order 2 (16KB): PFN 0-3 的伙伴是 PFN 4-7

  伙伴 PFN = pfn XOR (1 << order)
```

### 数据结构

```c
struct page {
    unsigned long flags;       /* PAGE_FREE 或 PAGE_RESERVED */
    struct list_head lru;      /* 链表节点 */
    unsigned int order;        /* 当前块的 order */
};

struct free_area {
    struct list_head free_list; /* 该 order 的空闲链表 */
    unsigned long nr_free;      /* 空闲块数 */
};

struct zone {
    struct free_area free_area[11]; /* order 0-10 */
    unsigned long free_pages;
    unsigned long total_pages;
};
```

### 分配流程

```c
struct page *alloc_pages(unsigned int order)
{
    /* 从 order 开始向上搜索可用块 */
    for (current_order = order; current_order < MAX_ORDER; current_order++) {
        if (list_empty(&zone.free_area[current_order].free_list))
            continue;

        /* 找到了！从空闲链表摘下 */
        page = list_first_entry(...);
        list_del(&page->lru);
        zone.free_area[current_order].nr_free--;

        /* 如果块太大，分裂并归还多余的 */
        expand(&zone, page, order, current_order);

        page->flags = 0;  /* 标记已分配 */
        return page;
    }
    return NULL;  /* 内存不足 */
}
```

### expand() — 块分裂

```
  需要 order=0 (4KB)，但只有 order=2 (16KB) 的块：

  分裂前:
  ┌──────────────────────────┐
  │     order=2 (16KB)       │  PFN 100-103
  └──────────────────────────┘

  第一次分裂 (order 2 → 1):
  ┌────────────┬─────────────┐
  │ order=1    │  order=1    │  PFN 102-103 归还到 free_area[1]
  │ (继续分裂) │  (归还)     │
  └────────────┴─────────────┘

  第二次分裂 (order 1 → 0):
  ┌──────┬──────┬─────────────┐
  │ o=0  │ o=0  │   order=1   │
  │(返回)│(归还)│   (已归还)  │  PFN 101 归还到 free_area[0]
  └──────┴──────┴─────────────┘
```

```c
static void expand(struct zone *z, struct page *page,
                   int low, int high)
{
    while (high > low) {
        high--;
        /* 右半块 = 当前页 + 2^high */
        struct page *buddy = page + (1 << high);
        buddy->order = high;
        buddy->flags = PAGE_FREE;
        list_add(&buddy->lru, &z->free_area[high].free_list);
        z->free_area[high].nr_free++;
    }
    page->order = low;
}
```

### 释放与合并

```c
void free_pages(struct page *page, unsigned int order)
{
    unsigned long pfn = page - mem_map;  /* 计算 PFN */

    while (order < MAX_ORDER - 1) {
        /* 计算伙伴 PFN */
        unsigned long buddy_pfn = pfn ^ (1UL << order);
        struct page *buddy = &mem_map[buddy_pfn];

        /* 伙伴空闲且同 order 才能合并 */
        if (!(buddy->flags & PAGE_FREE) || buddy->order != order)
            break;

        /* 从空闲链表摘下伙伴 */
        list_del(&buddy->lru);
        zone.free_area[order].nr_free--;

        /* 合并：取较小的 PFN */
        if (buddy_pfn < pfn)
            pfn = buddy_pfn;
        page = &mem_map[pfn];

        order++;  /* 尝试更高 order 合并 */
    }

    /* 放入合并后的 order 的空闲链表 */
    page->order = order;
    page->flags = PAGE_FREE;
    list_add(&page->lru, &zone.free_area[order].free_list);
    zone.free_area[order].nr_free++;
}
```

合并示例：

```
  释放 PFN 100 (order=0):
    伙伴 = 100 XOR 1 = 101，如果 101 也空闲:
    → 合并为 PFN 100 order=1

  继续检查:
    伙伴 = 100 XOR 2 = 102，如果 102-103 也空闲 (order=1):
    → 合并为 PFN 100 order=2
```

---

## 2.9 测试验证

```c
void test_buddy(void)
{
    struct page *p1, *p2, *p3;

    /* 测试 1: 单页分配 */
    p1 = alloc_pages(0);       /* 4KB */
    /* 验证 p1 != NULL */

    /* 测试 2: 多页分配 */
    p2 = alloc_pages(2);       /* 16KB = 4 pages */

    /* 测试 3: 释放并验证合并 */
    free_pages(p1, 0);
    free_pages(p2, 2);
    /* 如果 p1 和 p2 是伙伴，会自动合并 */

    /* 测试 4: 再次分配，验证合并后的大块可用 */
    p3 = alloc_pages(2);
    /* p3 应该能分配到（合并后释放了足够的连续空间）*/
}
```

---

## 2.10 start_kernel() 中的 Phase 2 代码

```c
void start_kernel(void)
{
    /* ... Phase 1 ... */

    /* Phase 2: MMU */
    boot_printk("[BOOT] Initializing MMU (identity mapping)...\n");
    mmu_init();
    boot_printk("[BOOT] MMU enabled (SCTLR_EL1.M = 1)\n");

    /* 验证 MMU 已开启 */
    unsigned long sctlr;
    __asm__ volatile("mrs %0, sctlr_el1" : "=r"(sctlr));
    boot_printk("[BOOT] SCTLR_EL1.M = ");
    boot_printk((sctlr & 1) ? "1" : "0");  /* 应为 1 */

    /* Phase 2: 内存 */
    memblock_init(PHYS_OFFSET, PHYS_SIZE);  /* 0x40000000, 0x40000000 */
    buddy_init();
    test_buddy();
}
```

---

## 2.11 Phase 2 核心概念总结

| 概念 | 说明 |
|------|------|
| **恒等映射** | VA == PA，MMU 开启时不需要改代码 |
| **L1 块描述符** | 1GB 粒度映射，Phase 2 最简单方案 |
| **MAIR_EL1** | 定义 4 种内存属性（设备/普通/缓存策略） |
| **TCR_EL1** | 配置地址翻译参数（位宽/粒度/共享） |
| **TTBR0_EL1** | 页表基址寄存器 |
| **Memblock** | 引导阶段分配器，给 Buddy 分配元数据 |
| **Buddy 系统** | 2^n 分配，伙伴 XOR 计算，自动合并 |
| **struct page** | 每个物理页的元数据描述符 |

**Phase 2 奠定的基础**：有了 MMU 和内存分配器，后续 Phase 才能分配页表、栈、缓冲区。

---

## 2.12 完整源码清单

> Phase 2 在 Phase 1 基础上新增 10 个文件，修改 `kernel/main.c` 和 `Makefile`。

### 新增目录结构

```
arm64os/
├── arch/arm64/
│   ├── include/asm/
│   │   ├── memory.h      ← 新增
│   │   ├── pgtable.h     ← 新增
│   │   └── sysreg.h      ← 新增
│   ├── kernel/
│   │   ├── head.S         (不变)
│   │   └── entry.S        (不变)
│   └── mm/
│       ├── mmu.c          ← 新增
│       ├── proc.S         ← 新增
│       └── tlb.S          ← 新增
├── include/linux/
│   ├── types.h            (不变)
│   ├── list.h             ← 新增
│   └── io.h               ← 新增
├── kernel/
│   ├── main.c             ← 修改
│   └── printk.c           (不变)
├── mm/
│   ├── memblock.c         ← 新增
│   └── page_alloc.c       ← 新增
├── scripts/linker.ld      (不变)
└── Makefile               ← 修改
```

### 新增文件 1: `include/linux/list.h`

```c
/* SPDX-License-Identifier: GPL-2.0 */
/*
 * arm64os/include/linux/list.h
 *
 * 内核双向循环链表（Buddy 分配器使用）
 */

#ifndef __LINUX_LIST_H
#define __LINUX_LIST_H

struct list_head {
    struct list_head *next;
    struct list_head *prev;
};

static inline void INIT_LIST_HEAD(struct list_head *list)
{
    list->next = list;
    list->prev = list;
}

static inline int list_empty(const struct list_head *head)
{
    return head->next == head;
}

static inline void __list_add(struct list_head *new,
                               struct list_head *prev,
                               struct list_head *next)
{
    next->prev = new;
    new->next  = next;
    new->prev  = prev;
    prev->next = new;
}

static inline void list_add(struct list_head *new, struct list_head *head)
{
    __list_add(new, head, head->next);
}

static inline void list_add_tail(struct list_head *new, struct list_head *head)
{
    __list_add(new, head->prev, head);
}

static inline void __list_del(struct list_head *prev, struct list_head *next)
{
    next->prev = prev;
    prev->next = next;
}

static inline void list_del(struct list_head *entry)
{
    __list_del(entry->prev, entry->next);
    entry->next = (struct list_head *)0xDEAD000000000000UL;
    entry->prev = (struct list_head *)0xDEAD000000000100UL;
}

#define container_of(ptr, type, member) \
    ((type *)((char *)(ptr) - __builtin_offsetof(type, member)))

#define list_entry(ptr, type, member) \
    container_of(ptr, type, member)

#define list_first_entry(head, type, member) \
    list_entry((head)->next, type, member)

#define list_for_each(pos, head) \
    for (pos = (head)->next; pos != (head); pos = pos->next)

#define list_for_each_safe(pos, n, head) \
    for (pos = (head)->next, n = pos->next; \
         pos != (head); \
         pos = n, n = pos->next)

#endif /* __LINUX_LIST_H */
```

### 新增文件 2: `include/linux/io.h`

```c
/* SPDX-License-Identifier: GPL-2.0 */
/*
 * arm64os/include/linux/io.h
 *
 * MMIO 寄存器访问 + 内存屏障
 */

#ifndef __LINUX_IO_H
#define __LINUX_IO_H

#include <linux/types.h>

static inline u32 readl(volatile void *addr)
{
    return *(volatile u32 *)addr;
}

static inline void writel(u32 val, volatile void *addr)
{
    *(volatile u32 *)addr = val;
}

static inline u64 readq(volatile void *addr)
{
    return *(volatile u64 *)addr;
}

static inline void writeq(u64 val, volatile void *addr)
{
    *(volatile u64 *)addr = val;
}

static inline u8 readb(volatile void *addr)
{
    return *(volatile u8 *)addr;
}

static inline void writeb(u8 val, volatile void *addr)
{
    *(volatile u8 *)addr = val;
}

/* ARM64 内存屏障 */
#define mb()    __asm__ volatile("dmb ish"   ::: "memory")
#define wmb()   __asm__ volatile("dmb ishst" ::: "memory")
#define rmb()   __asm__ volatile("dmb ishld" ::: "memory")

#define cpu_relax() __asm__ volatile("yield" ::: "memory")

#endif /* __LINUX_IO_H */
```

### 新增文件 3: `arch/arm64/include/asm/memory.h`

```c
/* SPDX-License-Identifier: GPL-2.0 */
/*
 * arm64os/arch/arm64/include/asm/memory.h
 *
 * 虚拟/物理地址空间布局
 */

#ifndef __ASM_MEMORY_H
#define __ASM_MEMORY_H

/* 页大小 */
#define PAGE_SHIFT          12
#define PAGE_SIZE           (1UL << PAGE_SHIFT)     /* 4KB */
#define PAGE_MASK           (~(PAGE_SIZE - 1))

/* 48位虚拟地址 */
#define VA_BITS             48
#define T0SZ_VALUE          (64 - VA_BITS)

/* 页表层级（4KB粒度，48位VA）*/
#define PGDIR_SHIFT         39
#define PUD_SHIFT           30
#define PMD_SHIFT           21
#define PAGE_SHIFT_L3       12

#define PTRS_PER_PGD        512
#define PTRS_PER_PUD        512
#define PTRS_PER_PMD        512
#define PTRS_PER_PTE        512

#define PGD_SIZE            (1UL << PGDIR_SHIFT)    /* 512GB */
#define PUD_SIZE            (1UL << PUD_SHIFT)      /* 1GB */
#define PMD_SIZE            (1UL << PMD_SHIFT)      /* 2MB */

/* QEMU virt 物理内存布局 */
#define PHYS_OFFSET         0x40000000UL
#define PHYS_SIZE           0x40000000UL            /* 1GB */
#define PHYS_END            (PHYS_OFFSET + PHYS_SIZE)

#define KERNEL_PHYS_LOAD    0x40080000UL

/* 高虚拟地址（预留） */
#define KIMAGE_VADDR        0xFFFF000000000000UL
#define PAGE_OFFSET         0xFFFF800000000000UL

/* 恒等映射：VA == PA */
#define phys_to_virt(phys)  ((void *)(unsigned long)(phys))
#define virt_to_phys(virt)  ((unsigned long)(virt))

#define PAGE_ALIGN_DOWN(x)  ((x) & PAGE_MASK)
#define PAGE_ALIGN_UP(x)    (((x) + PAGE_SIZE - 1) & PAGE_MASK)

#define phys_to_pfn(phys)   (((phys) - PHYS_OFFSET) >> PAGE_SHIFT)
#define pfn_to_phys(pfn)    (((pfn) << PAGE_SHIFT) + PHYS_OFFSET)

#endif /* __ASM_MEMORY_H */
```

### 新增文件 4: `arch/arm64/include/asm/sysreg.h`

```c
/* SPDX-License-Identifier: GPL-2.0 */
/*
 * arm64os/arch/arm64/include/asm/sysreg.h
 *
 * ARMv8-A 系统寄存器定义
 */

#ifndef __ASM_SYSREG_H
#define __ASM_SYSREG_H

/* SCTLR_EL1 */
#define SCTLR_EL1_M        (1UL << 0)
#define SCTLR_EL1_A        (1UL << 1)
#define SCTLR_EL1_C        (1UL << 2)
#define SCTLR_EL1_I        (1UL << 12)

/* HCR_EL2 */
#define HCR_EL2_RW         (1UL << 31)

/* TCR_EL1 字段 */
#define TCR_T0SZ(x)        ((unsigned long)(x) << 0)
#define TCR_T1SZ(x)        ((unsigned long)(x) << 16)
#define TCR_TG0_4K         (0UL << 14)
#define TCR_TG1_4K         (2UL << 30)
#define TCR_IRGN0_WBWA     (1UL << 8)
#define TCR_ORGN0_WBWA     (1UL << 10)
#define TCR_SH0_INNER      (3UL << 12)
#define TCR_IRGN1_WBWA     (1UL << 24)
#define TCR_ORGN1_WBWA     (1UL << 26)
#define TCR_SH1_INNER      (3UL << 28)
#define TCR_IPS_48BIT      (5UL << 32)

/* MAIR_EL1 */
#define MAIR_ATTR_DEVICE_nGnRnE  0x00UL
#define MAIR_ATTR_DEVICE_nGnRE   0x04UL
#define MAIR_ATTR_NORMAL_NC      0x44UL
#define MAIR_ATTR_NORMAL         0xffUL
#define MAIR_ATTR(idx, attr)     ((attr) << ((idx) * 8))

#define MT_DEVICE_nGnRnE   0
#define MT_DEVICE_nGnRE    1
#define MT_NORMAL_NC       2
#define MT_NORMAL          3

/* DAIF */
#define DAIF_IRQ_BIT       (1 << 1)

/* CurrentEL */
#define CurrentEL_EL1      (1 << 2)
#define CurrentEL_EL2      (2 << 2)

#endif /* __ASM_SYSREG_H */
```

### 新增文件 5: `arch/arm64/include/asm/pgtable.h`

```c
/* SPDX-License-Identifier: GPL-2.0 */
/*
 * arm64os/arch/arm64/include/asm/pgtable.h
 *
 * ARMv8-A 页表项格式
 */

#ifndef __ASM_PGTABLE_H
#define __ASM_PGTABLE_H

#include <asm/memory.h>

/* 描述符类型 bits[1:0] */
#define PD_INVALID          0x0UL
#define PD_BLOCK            0x1UL
#define PD_TABLE            0x3UL
#define PD_PAGE             0x3UL

/* 低位属性 */
#define PD_ATTRINDX(n)      (((unsigned long)(n)) << 2)
#define PD_AP_RW_EL1        (0UL << 6)
#define PD_AP_RW_ALL        (1UL << 6)
#define PD_AP_RO_ALL        (3UL << 6)
#define PD_SH_NONE          (0UL << 8)
#define PD_SH_OUTER         (2UL << 8)
#define PD_SH_INNER         (3UL << 8)
#define PD_AF               (1UL << 10)
#define PD_NG               (1UL << 11)

/* 高位属性 */
#define PD_PXN              (1UL << 53)
#define PD_UXN              (1UL << 54)

/* 常用组合 */
#define PD_KERNEL_EXEC      (PD_AF | PD_SH_INNER | PD_AP_RW_EL1 | \
                             PD_UXN | PD_ATTRINDX(3))
#define PD_KERNEL_DATA      (PD_AF | PD_SH_INNER | PD_AP_RW_EL1 | \
                             PD_PXN | PD_UXN | PD_ATTRINDX(3))
#define PD_DEVICE           (PD_AF | PD_SH_OUTER | PD_AP_RW_EL1 | \
                             PD_PXN | PD_UXN | PD_ATTRINDX(0))

/* Block/Page/Table 描述符构建 */
#define mk_block_desc(phys, attrs)  \
    (((unsigned long)(phys) & ~((1UL << PUD_SHIFT) - 1)) | (attrs) | PD_BLOCK)

#define mk_block_desc_2m(phys, attrs)  \
    (((unsigned long)(phys) & ~((1UL << PMD_SHIFT) - 1)) | (attrs) | PD_BLOCK)

#define mk_page_desc(phys, attrs)  \
    (((unsigned long)(phys) & PAGE_MASK) | (attrs) | PD_PAGE)

#define mk_table_desc(next_table_phys)  \
    (((unsigned long)(next_table_phys) & PAGE_MASK) | PD_TABLE)

/* 页表项辅助宏 */
#define pte_valid(pte)      ((pte) & 1UL)
#define pte_is_table(pte)   (((pte) & 3UL) == PD_TABLE)
#define pte_is_block(pte)   (((pte) & 3UL) == PD_BLOCK)
#define table_phys(pte)     ((pte) & (~0UL << PAGE_SHIFT) & ((1UL << 48) - 1))

/* 页表索引 */
#define pgd_index(va)   (((unsigned long)(va) >> PGDIR_SHIFT) & (PTRS_PER_PGD - 1))
#define pud_index(va)   (((unsigned long)(va) >> PUD_SHIFT)   & (PTRS_PER_PUD - 1))
#define pmd_index(va)   (((unsigned long)(va) >> PMD_SHIFT)   & (PTRS_PER_PMD - 1))
#define pte_index(va)   (((unsigned long)(va) >> PAGE_SHIFT)  & (PTRS_PER_PTE - 1))

#endif /* __ASM_PGTABLE_H */
```

### 新增文件 6: `arch/arm64/mm/proc.S`

```asm
/* SPDX-License-Identifier: GPL-2.0 */
/*
 * arm64os/arch/arm64/mm/proc.S
 *
 * CPU MMU 初始化：MAIR_EL1 + TCR_EL1 + 开启 MMU
 */

#include <asm/sysreg.h>

    .section ".text", "ax"

    .global cpu_init
cpu_init:
    /* MAIR_EL1: 4 种内存属性 */
    ldr     x0, =( \
        (0x00UL << (0 * 8)) |   /* idx 0: Device-nGnRnE */ \
        (0x04UL << (1 * 8)) |   /* idx 1: Device-nGnRE  */ \
        (0x44UL << (2 * 8)) |   /* idx 2: Normal-NC     */ \
        (0xFFUL << (3 * 8))  )  /* idx 3: Normal-WBWA   */
    msr     mair_el1, x0

    /* TCR_EL1: 地址翻译控制 */
    ldr     x0, =( \
        (16UL  <<  0) |     /* T0SZ = 16 (48位VA) */ \
        (1UL   <<  8) |     /* IRGN0 = WB-RA      */ \
        (1UL   << 10) |     /* ORGN0 = WB-RA      */ \
        (3UL   << 12) |     /* SH0 = Inner Share   */ \
        (0UL   << 14) |     /* TG0 = 4KB           */ \
        (16UL  << 16) |     /* T1SZ = 16           */ \
        (1UL   << 24) |     /* IRGN1 = WB-RA      */ \
        (1UL   << 26) |     /* ORGN1 = WB-RA      */ \
        (3UL   << 28) |     /* SH1 = Inner Share   */ \
        (2UL   << 30) |     /* TG1 = 4KB           */ \
        (5UL   << 32)  )    /* IPS = 48位PA        */
    msr     tcr_el1, x0

    isb
    ret

    .global enable_mmu
enable_mmu:
    msr     ttbr0_el1, x0          /* TTBR0: 恒等映射页表 */
    msr     ttbr1_el1, xzr         /* TTBR1: 暂不使用 */

    dsb     ish
    isb

    /* 开启 MMU + D-Cache + I-Cache */
    mrs     x1, sctlr_el1
    orr     x1, x1, #(1 << 0)     /* M: MMU enable */
    orr     x1, x1, #(1 << 2)     /* C: D-cache */
    orr     x1, x1, #(1 << 12)    /* I: I-cache */
    msr     sctlr_el1, x1

    isb

    /* TLB 全局无效化 */
    tlbi    vmalle1
    dsb     ish
    isb
    ret
```

### 新增文件 7: `arch/arm64/mm/tlb.S`

```asm
/* SPDX-License-Identifier: GPL-2.0 */
/*
 * arm64os/arch/arm64/mm/tlb.S
 *
 * TLB 操作辅助函数
 */

    .section ".text", "ax"

    .global tlb_flush_all
tlb_flush_all:
    tlbi    vmalle1
    dsb     ish
    isb
    ret

    .global tlb_flush_page
tlb_flush_page:
    lsr     x0, x0, #12
    tlbi    vae1, x0
    dsb     ish
    isb
    ret
```

### 新增文件 8: `arch/arm64/mm/mmu.c`

```c
/* SPDX-License-Identifier: GPL-2.0 */
/*
 * arm64os/arch/arm64/mm/mmu.c
 *
 * MMU 初始化：恒等映射页表
 */

#include <linux/types.h>
#include <asm/memory.h>
#include <asm/pgtable.h>

/* 静态页表（4KB 对齐，BSS 段，已被 head.S 清零） */
static unsigned long init_pgd[PTRS_PER_PGD] __attribute__((aligned(4096)));
static unsigned long init_pud[PTRS_PER_PUD] __attribute__((aligned(4096)));

extern void cpu_init(void);
extern void enable_mmu(unsigned long pgd_phys);

static unsigned long create_page_tables(void)
{
    unsigned long pgd_phys = (unsigned long)init_pgd;
    unsigned long pud_phys = (unsigned long)init_pud;

    /* L1[0]: 设备内存 [0x0, 0x40000000) */
    init_pud[pud_index(0x00000000UL)] =
        mk_block_desc(0x00000000UL,
                      PD_ATTRINDX(0) | PD_SH_OUTER | PD_AF |
                      PD_PXN | PD_UXN);

    /* L1[1]: RAM [0x40000000, 0x80000000) */
    init_pud[pud_index(0x40000000UL)] =
        mk_block_desc(0x40000000UL,
                      PD_ATTRINDX(3) | PD_SH_INNER | PD_AF);

    /* L0[0] → PUD */
    init_pgd[pgd_index(0x40000000UL)] =
        mk_table_desc(pud_phys);

    __asm__ volatile("dsb ish" ::: "memory");
    return pgd_phys;
}

void mmu_init(void)
{
    unsigned long pgd_phys = create_page_tables();
    cpu_init();
    enable_mmu(pgd_phys);
}

unsigned long get_kernel_pgd(void)
{
    return (unsigned long)init_pgd;
}
```

### 新增文件 9: `mm/memblock.c`

```c
/* SPDX-License-Identifier: GPL-2.0 */
/*
 * arm64os/mm/memblock.c
 *
 * 早期物理内存分配器
 */

#include <linux/types.h>
#include <asm/memory.h>

void boot_printk(const char *s);
void boot_printk_hex(unsigned long val);

#define MEMBLOCK_MAX_REGIONS    16

struct memblock_region {
    phys_addr_t base;
    phys_addr_t size;
};

struct memblock_type {
    unsigned int        cnt;
    struct memblock_region  regions[MEMBLOCK_MAX_REGIONS];
};

static struct memblock {
    struct memblock_type    memory;
    struct memblock_type    reserved;
} memblock_data;

static int region_add(struct memblock_type *type,
                      phys_addr_t base, phys_addr_t size)
{
    if (type->cnt >= MEMBLOCK_MAX_REGIONS) {
        boot_printk("[memblock] ERROR: too many regions\n");
        return -1;
    }
    type->regions[type->cnt].base = base;
    type->regions[type->cnt].size = size;
    type->cnt++;
    return 0;
}

static int regions_overlap(phys_addr_t base1, phys_addr_t size1,
                            phys_addr_t base2, phys_addr_t size2)
{
    return (base1 < base2 + size2) && (base2 < base1 + size1);
}

int memblock_add(phys_addr_t base, phys_addr_t size)
{
    return region_add(&memblock_data.memory, base, size);
}

int memblock_reserve(phys_addr_t base, phys_addr_t size)
{
    return region_add(&memblock_data.reserved, base, size);
}

phys_addr_t memblock_alloc(phys_addr_t size, phys_addr_t align)
{
    struct memblock_type *mem = &memblock_data.memory;
    struct memblock_type *res = &memblock_data.reserved;
    unsigned int i;

    if (align < PAGE_SIZE)
        align = PAGE_SIZE;
    size = (size + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);

    for (i = 0; i < mem->cnt; i++) {
        phys_addr_t region_base = mem->regions[i].base;
        phys_addr_t region_end  = region_base + mem->regions[i].size;
        phys_addr_t candidate;
        unsigned int j;
        int conflict;

        candidate = (region_base + align - 1) & ~(align - 1);

        while (candidate + size <= region_end) {
            conflict = 0;
            for (j = 0; j < res->cnt; j++) {
                if (regions_overlap(candidate, size,
                                    res->regions[j].base,
                                    res->regions[j].size)) {
                    candidate = res->regions[j].base + res->regions[j].size;
                    candidate = (candidate + align - 1) & ~(align - 1);
                    conflict = 1;
                    break;
                }
            }
            if (!conflict) {
                memblock_reserve(candidate, size);
                return candidate;
            }
        }
    }

    boot_printk("[memblock] ERROR: allocation failed\n");
    return 0;
}

void memblock_for_each_free_region(void (*fn)(phys_addr_t base, phys_addr_t size))
{
    struct memblock_type *mem = &memblock_data.memory;
    struct memblock_type *res = &memblock_data.reserved;
    unsigned int i, j;

    for (i = 0; i < mem->cnt; i++) {
        phys_addr_t region_base = mem->regions[i].base;
        phys_addr_t region_end  = region_base + mem->regions[i].size;
        phys_addr_t cursor;

        struct {
            phys_addr_t base;
            phys_addr_t end;
        } overlap[MEMBLOCK_MAX_REGIONS];
        int noverlap = 0;
        int k;

        for (j = 0; j < res->cnt; j++) {
            phys_addr_t rb = res->regions[j].base;
            phys_addr_t re = rb + res->regions[j].size;
            if (rb < region_end && re > region_base) {
                overlap[noverlap].base = (rb > region_base) ? rb : region_base;
                overlap[noverlap].end  = (re < region_end)  ? re : region_end;
                noverlap++;
            }
        }

        /* 冒泡排序 */
        for (j = 0; j < (unsigned int)noverlap - 1; j++) {
            for (k = 0; k < noverlap - 1 - (int)j; k++) {
                if (overlap[k].base > overlap[k+1].base) {
                    phys_addr_t tb = overlap[k].base;
                    phys_addr_t te = overlap[k].end;
                    overlap[k].base = overlap[k+1].base;
                    overlap[k].end  = overlap[k+1].end;
                    overlap[k+1].base = tb;
                    overlap[k+1].end  = te;
                }
            }
        }

        cursor = region_base;
        for (k = 0; k < noverlap; k++) {
            if (cursor < overlap[k].base)
                fn(cursor, overlap[k].base - cursor);
            if (overlap[k].end > cursor)
                cursor = overlap[k].end;
        }
        if (cursor < region_end)
            fn(cursor, region_end - cursor);
    }
}

void memblock_init(phys_addr_t phys_start, phys_addr_t phys_size)
{
    extern char _text[];
    extern char _end[];

    phys_addr_t kernel_start = (phys_addr_t)(unsigned long)_text;
    phys_addr_t kernel_end   = (phys_addr_t)(unsigned long)_end;
    kernel_end = (kernel_end + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);

    memblock_add(phys_start, phys_size);
    memblock_reserve(kernel_start, kernel_end - kernel_start);

    if (kernel_start > phys_start)
        memblock_reserve(phys_start, kernel_start - phys_start);

    boot_printk("[memblock] initialized: RAM ");
    boot_printk_hex(phys_start);
    boot_printk(" - ");
    boot_printk_hex(phys_start + phys_size);
    boot_printk("\n");
}
```

### 新增文件 10: `mm/page_alloc.c`

```c
/* SPDX-License-Identifier: GPL-2.0 */
/*
 * arm64os/mm/page_alloc.c
 *
 * Buddy 物理内存分配器
 */

#include <linux/types.h>
#include <linux/list.h>
#include <asm/memory.h>

void boot_printk(const char *s);
void boot_printk_hex(unsigned long val);

void memblock_for_each_free_region(void (*fn)(phys_addr_t base, phys_addr_t size));
phys_addr_t memblock_alloc(phys_addr_t size, phys_addr_t align);

#define MAX_ORDER           11
#define MAX_PAGES           (PHYS_SIZE >> PAGE_SHIFT)

struct page {
    unsigned long       flags;
    struct list_head    lru;
    unsigned int        order;
    unsigned int        _pad;
};

#define PAGE_FREE           (1UL << 0)
#define PAGE_RESERVED       (1UL << 1)

struct free_area {
    struct list_head    free_list;
    unsigned long       nr_free;
};

struct zone {
    struct free_area    free_area[MAX_ORDER];
    unsigned long       free_pages;
    unsigned long       total_pages;
};

static struct page *page_array;
static struct zone main_zone;

static inline unsigned long page_to_pfn(const struct page *page)
{
    return (unsigned long)(page - page_array);
}

static inline struct page *pfn_to_page(unsigned long pfn)
{
    return &page_array[pfn];
}

static inline phys_addr_t page_to_phys(const struct page *page)
{
    return pfn_to_phys(page_to_pfn(page));
}

static inline unsigned long pfn_buddy(unsigned long pfn, unsigned int order)
{
    return pfn ^ (1UL << order);
}

static inline int page_is_buddy(struct page *page, struct page *buddy,
                                  unsigned int order)
{
    unsigned long buddy_pfn = page_to_pfn(buddy);
    if (buddy_pfn >= main_zone.total_pages)
        return 0;
    if (!(buddy->flags & PAGE_FREE))
        return 0;
    if (buddy->order != order)
        return 0;
    return 1;
}

static void __free_pages_ok(struct page *page, unsigned int order)
{
    unsigned long pfn = page_to_pfn(page);

    while (order < MAX_ORDER - 1) {
        unsigned long buddy_pfn = pfn_buddy(pfn, order);
        struct page *buddy = pfn_to_page(buddy_pfn);

        if (!page_is_buddy(page, buddy, order))
            break;

        list_del(&buddy->lru);
        main_zone.free_area[order].nr_free--;
        buddy->flags &= ~PAGE_FREE;

        if (buddy_pfn < pfn) {
            page = buddy;
            pfn = buddy_pfn;
        }
        order++;
    }

    page->order = order;
    page->flags |= PAGE_FREE;
    list_add(&page->lru, &main_zone.free_area[order].free_list);
    main_zone.free_area[order].nr_free++;
}

static void expand(struct page *page, unsigned int low_order,
                   unsigned int high_order)
{
    unsigned long pfn = page_to_pfn(page);
    unsigned int order = high_order;

    while (order > low_order) {
        unsigned long buddy_pfn;
        struct page *buddy;

        order--;
        buddy_pfn = pfn + (1UL << order);
        buddy = pfn_to_page(buddy_pfn);
        buddy->order = order;
        buddy->flags |= PAGE_FREE;
        list_add(&buddy->lru, &main_zone.free_area[order].free_list);
        main_zone.free_area[order].nr_free++;
    }
}

static struct page *__rmqueue_smallest(unsigned int order)
{
    unsigned int current_order;
    struct free_area *area;
    struct page *page;

    for (current_order = order; current_order < MAX_ORDER; current_order++) {
        area = &main_zone.free_area[current_order];
        if (list_empty(&area->free_list))
            continue;

        page = list_first_entry(&area->free_list, struct page, lru);
        list_del(&page->lru);
        area->nr_free--;
        page->flags &= ~PAGE_FREE;

        if (current_order > order)
            expand(page, order, current_order);

        page->order = order;
        return page;
    }
    return (struct page *)0;
}

struct page *alloc_pages(unsigned int order)
{
    struct page *page;
    if (order >= MAX_ORDER)
        return (struct page *)0;
    page = __rmqueue_smallest(order);
    if (page)
        main_zone.free_pages -= (1UL << order);
    return page;
}

void __free_pages(struct page *page, unsigned int order)
{
    if (!page || order >= MAX_ORDER)
        return;
    main_zone.free_pages += (1UL << order);
    __free_pages_ok(page, order);
}

void *page_address(struct page *page)
{
    return (void *)(unsigned long)page_to_phys(page);
}

static void free_memblock_region(phys_addr_t base, phys_addr_t size)
{
    phys_addr_t end = base + size;
    phys_addr_t pa;

    base = (base + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    end  = end & ~(PAGE_SIZE - 1);

    for (pa = base; pa < end; pa += PAGE_SIZE) {
        unsigned long pfn = phys_to_pfn(pa);
        if (pfn < main_zone.total_pages) {
            struct page *page = pfn_to_page(pfn);
            page->flags = 0;
            page->order = 0;
            INIT_LIST_HEAD(&page->lru);
            __free_pages_ok(page, 0);
            main_zone.free_pages++;
        }
    }
}

void buddy_init(void)
{
    phys_addr_t page_array_size;
    phys_addr_t page_array_phys;
    unsigned int i;
    unsigned long pfn;

    page_array_size = MAX_PAGES * sizeof(struct page);
    page_array_phys = memblock_alloc(page_array_size, PAGE_SIZE);

    if (!page_array_phys) {
        boot_printk("[buddy] FATAL: cannot allocate page array\n");
        while (1);
    }

    page_array = (struct page *)(unsigned long)page_array_phys;

    main_zone.total_pages = MAX_PAGES;
    main_zone.free_pages  = 0;
    for (i = 0; i < MAX_ORDER; i++) {
        INIT_LIST_HEAD(&main_zone.free_area[i].free_list);
        main_zone.free_area[i].nr_free = 0;
    }

    for (pfn = 0; pfn < MAX_PAGES; pfn++) {
        page_array[pfn].flags = PAGE_RESERVED;
        page_array[pfn].order = 0;
        INIT_LIST_HEAD(&page_array[pfn].lru);
    }

    memblock_for_each_free_region(free_memblock_region);

    boot_printk("[buddy] initialized: ");
    boot_printk_hex(main_zone.free_pages);
    boot_printk(" free pages\n");
}

void test_buddy(void)
{
    struct page *p1, *p2, *p3;
    unsigned long free_before, free_after;

    boot_printk("[buddy] === test_buddy start ===\n");
    free_before = main_zone.free_pages;

    p1 = alloc_pages(0);
    if (!p1) { boot_printk("[buddy] FAIL: alloc order=0\n"); return; }
    boot_printk("[buddy] alloc order=0: PA=");
    boot_printk_hex(page_to_phys(p1));
    boot_printk("\n");

    p2 = alloc_pages(2);
    if (!p2) { boot_printk("[buddy] FAIL: alloc order=2\n"); __free_pages(p1, 0); return; }

    p3 = alloc_pages(4);
    if (p3) {
        __free_pages(p3, 4);
    }

    __free_pages(p2, 2);
    __free_pages(p1, 0);

    free_after = main_zone.free_pages;
    if (free_after == free_before)
        boot_printk("[buddy] memory fully reclaimed: PASS\n");

    boot_printk("[buddy] === test_buddy end ===\n");
}
```

### 修改文件: `kernel/main.c` (Phase 2 版本)

> 在 Phase 1 的 `start_kernel()` 末尾 `while(1)` 之前，添加 Phase 2 代码。

```c
/* SPDX-License-Identifier: GPL-2.0 */
/*
 * arm64os/kernel/main.c — Phase 2 版本
 */

#include <linux/types.h>
#include <asm/memory.h>

void boot_printk(const char *s);
void boot_printk_hex(unsigned long val);

/* Phase 2 */
void mmu_init(void);
void memblock_init(phys_addr_t phys_start, phys_addr_t phys_size);
void buddy_init(void);
void test_buddy(void);

extern char _text[];
extern char _end[];
extern char _bss_start[];
extern char _bss_end[];
extern unsigned long boot_args[4];

struct pt_regs {
    unsigned long regs[31];
    unsigned long sp;
    unsigned long pc;
    unsigned long pstate;
};

static const char *esr_to_str(unsigned long esr)
{
    unsigned int ec = (esr >> 26) & 0x3f;
    switch (ec) {
    case 0x00: return "Unknown reason";
    case 0x15: return "SVC (AArch64 syscall)";
    case 0x20: return "Instruction Abort (lower EL)";
    case 0x21: return "Instruction Abort (current EL)";
    case 0x24: return "Data Abort (lower EL)";
    case 0x25: return "Data Abort (current EL)";
    default:   return "Unknown EC";
    }
}

void handle_sync_exception(struct pt_regs *regs)
{
    unsigned long esr, far;
    __asm__ volatile("mrs %0, esr_el1" : "=r"(esr));
    __asm__ volatile("mrs %0, far_el1" : "=r"(far));

    boot_printk("\n[EXCEPTION] Synchronous exception!\n");
    boot_printk("  ESR_EL1: ");
    boot_printk_hex(esr);
    boot_printk(" (");
    boot_printk(esr_to_str(esr));
    boot_printk(")\n");
    boot_printk("  FAR_EL1: ");
    boot_printk_hex(far);
    boot_printk("\n  PC     : ");
    boot_printk_hex(regs->pc);
    boot_printk("\n");

    boot_printk("[PANIC] Unrecoverable — halting.\n");
    while (1);
}

void panic_unhandled(void)
{
    boot_printk("\n[PANIC] Unhandled exception!\n");
    while (1);
}

void start_kernel(void)
{
    boot_printk("[BOOT] ARM64 kernel starting...\n");
    boot_printk("[BOOT] Phase 2: MMU + Buddy allocator\n");

    /* Phase 1: 打印启动信息 */
    boot_printk("[BOOT] Kernel text   : ");
    boot_printk_hex((unsigned long)_text);
    boot_printk("\n");
    boot_printk("[BOOT] Kernel end    : ");
    boot_printk_hex((unsigned long)_end);
    boot_printk("\n");
    boot_printk("[BOOT] FDT addr      : ");
    boot_printk_hex(boot_args[0]);
    boot_printk("\n");

    {
        unsigned long vbar;
        __asm__ volatile("mrs %0, vbar_el1" : "=r"(vbar));
        boot_printk("[BOOT] VBAR_EL1      : ");
        boot_printk_hex(vbar);
        boot_printk("\n");
    }

    /* ---- Phase 2: MMU 初始化 ---- */
    boot_printk("[BOOT] Initializing MMU (identity mapping)...\n");
    mmu_init();
    boot_printk("[BOOT] MMU enabled (SCTLR_EL1.M = 1)\n");

    {
        unsigned long sctlr;
        __asm__ volatile("mrs %0, sctlr_el1" : "=r"(sctlr));
        boot_printk("[BOOT] SCTLR_EL1     : ");
        boot_printk_hex(sctlr);
        boot_printk(" (M=");
        boot_printk((sctlr & 1) ? "1" : "0");
        boot_printk(")\n");
    }

    /* ---- Phase 2: 物理内存初始化 ---- */
    boot_printk("[BOOT] Initializing memblock...\n");
    memblock_init(PHYS_OFFSET, PHYS_SIZE);

    boot_printk("[BOOT] Initializing buddy allocator...\n");
    buddy_init();

    /* ---- Phase 2: 验证 ---- */
    test_buddy();

    boot_printk("[BOOT] Phase 2 complete\n");

    while (1);
}
```

### 修改文件: `Makefile` (Phase 2 版本)

> 在 Phase 1 的 Makefile 基础上添加新的目标文件和编译规则。

OBJS 修改为:
```makefile
OBJS := \
    arch/arm64/kernel/head.o \
    arch/arm64/kernel/entry.o \
    arch/arm64/mm/proc.o \
    arch/arm64/mm/tlb.o \
    arch/arm64/mm/mmu.o \
    mm/memblock.o \
    mm/page_alloc.o \
    kernel/printk.o \
    kernel/main.o
```

新增编译规则:
```makefile
# arch/arm64/mm 汇编
arch/arm64/mm/%.o: arch/arm64/mm/%.S
	$(CC) $(ASFLAGS) -x assembler-with-cpp -c -o $@ $<

# arch/arm64/mm C 文件
arch/arm64/mm/%.o: arch/arm64/mm/%.c
	$(CC) $(CFLAGS) -c -o $@ $<

# mm/ C 文件
mm/%.o: mm/%.c
	$(CC) $(CFLAGS) -c -o $@ $<
```

### 编译运行

```bash
# 创建新目录
mkdir -p arch/arm64/mm arch/arm64/include/asm mm

make clean && make
make run
# 期望输出包含：
#   [BOOT] MMU enabled (SCTLR_EL1.M = 1)
#   [buddy] memory fully reclaimed: PASS
#   [BOOT] Phase 2 complete
```
