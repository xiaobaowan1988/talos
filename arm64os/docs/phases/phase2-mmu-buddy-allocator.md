# Phase 2：MMU建立 + 4级页表 + Buddy内存分配器

## 参考内核文件

```
arch/arm64/mm/mmu.c               # MMU初始化、页表建立
arch/arm64/mm/proc.S              # __cpu_setup, __enable_mmu
arch/arm64/include/asm/pgtable.h  # 页表项格式定义
arch/arm64/include/asm/memory.h   # 虚拟地址空间布局
mm/page_alloc.c                   # Buddy分配器核心
mm/memblock.c                     # 早期物理内存分配器
```

---

## 2.1 ARMv8 4级页表结构（4KB页，48位VA）

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
#define MT_NORMAL        0  /* 普通内存（可缓存）*/
#define MT_NORMAL_NC     1  /* 普通内存（不可缓存）*/
#define MT_DEVICE_nGnRnE 2  /* 设备内存（MMIO）*/

/* MAIR_EL1 配置 */
#define MAIR_EL1_VALUE  \
    (0xFFUL << (8 * MT_NORMAL))    | \  /* 普通内存 */
    (0x44UL << (8 * MT_NORMAL_NC)) | \  /* 普通内存NC */
    (0x00UL << (8 * MT_DEVICE_nGnRnE))  /* 设备内存 */
```

## 2.2 早期页表建立（参考 mmu.c __create_page_tables）

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

## 2.3 Buddy分配器设计（参考 mm/page_alloc.c）

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

## 2.4 关键难点与注意事项

1. **identity map窗口**：MMU开启瞬间CPU还在物理地址执行，必须保证物理地址和虚拟地址都能访问到同一条指令（`isb`之前用identity map，之后跳转到KIMAGE_VADDR高地址）。

2. **KASLR简化**：教学实现可以固定加载地址，不实现随机化。

3. **TLB shootdown**：单核不需要，多核时修改页表后必须广播TLB无效化（`dsb ishst` + `tlbi vmalle1is` + `dsb ish` + `isb`）。

4. **memblock vs buddy**：MMU开启前用`memblock`分配早期内存（简单线性分配），buddy初始化后才能动态管理。

5. **struct page内存**：1GB物理内存需要约16MB的`struct page`数组（1GB/4KB * 64字节/struct page = 16MB）。

## 2.5 验证方法

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

## 2.6 本阶段产出文件

```
arm64os/
├── arch/arm64/mm/mmu.c           ← MMU初始化（核心）
├── arch/arm64/mm/tlb.S           ← TLB操作辅助
├── arch/arm64/include/asm/
│   ├── pgtable.h                 ← 页表项定义
│   └── memory.h                  ← 虚拟地址空间布局
├── mm/
│   ├── memblock.c                ← 早期物理内存分配
│   └── page_alloc.c              ← Buddy分配器核心
└── Makefile
```
