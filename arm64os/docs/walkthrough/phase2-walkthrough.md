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
