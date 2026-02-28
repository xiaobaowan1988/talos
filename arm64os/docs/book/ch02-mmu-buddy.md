# Phase 2：MMU 页表 + Buddy 分配器

## 知识来源总览

- **ARMv8-A 手册 (D5章 MMU)**：约 40%（页表格式、MAIR、TCR、TTBR、TLB flush 序列）
- **Linux 伙伴分配器算法**：约 25%（free_area、split/merge、XOR buddy）
- **Phase 2 文档**：约 20%（简化策略、identity mapping 决策）
- **工程常识**：约 15%（memblock 早期分配器、构建系统修改）

## memory.h 虚拟地址布局

```c
#define VA_BITS    48
#define PAGE_SHIFT 12
#define PAGE_SIZE  (1UL << PAGE_SHIFT)    /* 4096 */
#define PHYS_OFFSET 0x40000000UL

#define __pa(x) ((unsigned long)(x))      /* VA → PA */
#define __va(x) ((void *)(unsigned long)(x)) /* PA → VA */
```

**`VA_BITS = 48`**：ARMv8-A 支持 48 位虚拟地址空间（256TB）。TCR_EL1 中的 T0SZ/T1SZ 字段控制有效位数：`T0SZ = 64 - VA_BITS = 16`。

**`PAGE_SIZE = 4096`**：ARM64 支持 4KB、16KB、64KB 三种页大小。4KB 是最常用的（与 x86 一致），也是 Linux 的默认选择。

**`__pa(x) = x`（identity mapping）**：虚拟地址 == 物理地址。这是 Phase 2 的简化决策——真实内核有 PHYS_OFFSET 到 PAGE_OFFSET 的线性映射偏移，但对于教学内核，identity mapping 大幅简化了页表构建。

## pgtable.h 页表项定义

```c
#define PTE_TYPE_BLOCK  0x1UL   /* 块描述符（1GB/2MB 大页）*/
#define PTE_TYPE_TABLE  0x3UL   /* 表描述符（指向下级页表）*/
#define PTE_AF          (1UL << 10)  /* 访问标志 */
```

**来源：ARMv8-A ARM, D5.3 Translation table descriptor formats**。

ARM64 页表项的低 2 位决定类型：
- `00` = 无效
- `01` = Block（当前级别直接映射一大块内存）
- `11` = Table（指向下一级页表）

**PTE_AF（Access Flag）**：bit 10 = 1 表示页面已被访问。如果 AF=0 且硬件不支持自动设置，首次访问会触发 Access Flag Fault。设为 1 避免此异常。

### MAIR 值推导

```c
#define MAIR_EL1_VALUE  ((0x00UL << 0) |   /* Attr0: Device-nGnRnE */
                         (0x44UL << 8) |   /* Attr1: Normal Non-cacheable */
                         (0xFFUL << 16))   /* Attr2: Normal Write-Back */
```

**来源：ARMv8-A ARM, D13.2.97 MAIR_EL1**。

MAIR（Memory Attribute Indirection Register）定义最多 8 种内存属性。页表项的 AttrIndx 字段（bit[4:2]）选择使用哪种属性。

- **Attr0 = 0x00 = Device-nGnRnE**：设备内存。nG（非 Gathering）= 不合并多次访问；nR（非 Reordering）= 不乱序；nE（非 Early write acknowledgement）= 写入必须到达设备才确认。用于 MMIO 寄存器（UART、GIC）。
- **Attr1 = 0x44 = Normal Non-cacheable**：普通内存但不缓存。Phase 2 使用此属性简化实现。
- **Attr2 = 0xFF = Normal Write-Back**：普通内存，写回缓存。这是性能最好的属性，正常 RAM 应该使用此属性。

### TCR_EL1 值推导

```c
#define TCR_T0SZ_48   (16UL << 0)    /* TTBR0: 48-bit VA */
#define TCR_T1SZ_48   (16UL << 16)   /* TTBR1: 48-bit VA */
#define TCR_TG0_4K    (0UL  << 14)   /* TTBR0: 4KB 粒度 */
#define TCR_TG1_4K    (2UL  << 30)   /* TTBR1: 4KB 粒度 */
```

**来源：ARMv8-A ARM, D13.2.131 TCR_EL1**。

TCR（Translation Control Register）控制页表翻译的参数。

**T0SZ = 16**：`64 - 16 = 48` 位有效虚拟地址。TTBR0 管理地址空间的低半部分（用户空间 0x0000_0000_0000_0000 到 0x0000_FFFF_FFFF_FFFF）。

**TG0 编码**：`00` = 4KB，`01` = 64KB，`10` = 16KB。注意 TG0 和 TG1 的编码不同——TG1 中 `10` = 4KB。这是 ARM 规范中容易出错的地方。

## mmu.c 页表构建

### create_page_tables

```c
void create_page_tables(void) {
    /* PGD[0] → PUD 表 */
    pgd[0] = (unsigned long)pud | PTE_TYPE_TABLE;

    /* PUD[0] = 0x0000_0000 ~ 0x3FFF_FFFF : Device (1GB block) */
    pud[0] = (0x00000000UL) |
             PTE_TYPE_BLOCK | PTE_AF |
             PTE_ATTRINDX(0) |  /* Device-nGnRnE */
             PTE_SH_OSH;

    /* PUD[1] = 0x4000_0000 ~ 0x7FFF_FFFF : Normal (1GB block) */
    pud[1] = (0x40000000UL) |
             PTE_TYPE_BLOCK | PTE_AF |
             PTE_ATTRINDX(2) |  /* Normal Write-Back */
             PTE_SH_ISH;
}
```

**为什么用 1GB 块描述符？**

ARM64 的 4 级页表：PGD → PUD → PMD → PTE。每级覆盖的地址范围：
- PGD 条目 = 512GB
- PUD 条目 = 1GB
- PMD 条目 = 2MB
- PTE 条目 = 4KB

在 PUD 级别使用块描述符（`PTE_TYPE_BLOCK = 0x1`）直接映射 1GB，不需要 PMD 和 PTE 级别。只用 2 个 PUD 条目就映射了整个 2GB 地址空间。

**0x00000000 ~ 0x3FFFFFFF = 设备区域**：UART (0x09000000)、GIC (0x08000000) 等 MMIO 寄存器都在第一个 1GB 范围内。

**0x40000000 ~ 0x7FFFFFFF = RAM 区域**：QEMU virt 的内存从 0x40000000 开始。

### enable_mmu

```c
void enable_mmu(void) {
    /* 1. 设置 MAIR */
    asm volatile("msr mair_el1, %0" :: "r"(MAIR_EL1_VALUE));

    /* 2. 设置 TCR */
    asm volatile("msr tcr_el1, %0" :: "r"(TCR_EL1_VALUE));

    /* 3. 设置页表基地址 */
    asm volatile("msr ttbr0_el1, %0" :: "r"(pgd));

    /* 4. TLB flush */
    tlb_flush_all();

    /* 5. 开启 MMU */
    unsigned long sctlr;
    asm volatile("mrs %0, sctlr_el1" : "=r"(sctlr));
    sctlr |= (1 << 0);   /* M = MMU enable */
    sctlr |= (1 << 2);   /* C = Data cache enable */
    sctlr |= (1 << 12);  /* I = Instruction cache enable */
    asm volatile("msr sctlr_el1, %0" :: "r"(sctlr));
    asm volatile("isb");
}
```

**顺序至关重要**：必须先设 MAIR → TCR → TTBR → TLB flush → 最后开 MMU。如果先开 MMU 再设页表基址，处理器会用旧的（未初始化的）TTBR 做地址翻译导致崩溃。

**`isb` 在最后**：确保 SCTLR_EL1 的写入在后续指令获取前生效。MMU 开启后的第一条指令的获取必须经过新启用的 MMU 翻译。

## tlb.S TLB 刷新

```asm
tlb_flush_all:
    dsb ishst       /* 确保页表写入完成 */
    tlbi vmalle1    /* 使所有 EL1 TLB 条目无效 */
    dsb ish         /* 等待 TLB 失效完成 */
    isb             /* 同步指令流 */
    ret
```

**来源：ARMv8-A ARM, D4.8 TLB maintenance**。

这四条指令的顺序是 ARM 手册规定的标准 TLB 维护序列：

1. **`dsb ishst`**（Data Synchronization Barrier, Inner Shareable, Store）：确保之前对页表的所有 store 操作完成，所有 CPU 核心都能看到新的页表内容。
2. **`tlbi vmalle1`**（TLB Invalidate, VM All, EL1）：使所有 EL1 的 TLB 条目无效。
3. **`dsb ish`**（Data Synchronization Barrier, Inner Shareable）：等待 TLB 失效操作在所有核心上完成。
4. **`isb`**（Instruction Synchronization Barrier）：确保后续的指令获取使用新的 TLB 条目。

## Buddy 分配器

### 核心数据结构

```c
struct free_area {
    struct list_head free_list;
    unsigned long    nr_free;
};

static struct free_area free_area[MAX_ORDER + 1];
```

**来源：Linux 内核 `mm/page_alloc.c`**。

Buddy 算法（1963 年 Harry Markowitz 提出，Kenneth Knowlton 完善）的核心思想：将内存按 2 的幂次大小组织，分配时从大块拆分，释放时与"伙伴"合并。

`MAX_ORDER = 10` → 最大块 = 2^10 页 = 4MB。`free_area[order]` 中的每个节点是一个 2^order 页的空闲块。

### alloc_pages

```c
struct page *alloc_pages(unsigned int order) {
    for (int current_order = order;
         current_order <= MAX_ORDER; current_order++) {
        if (list_empty(&free_area[current_order].free_list))
            continue;

        page = list_first_entry(...);
        list_del(&page->lru);
        free_area[current_order].nr_free--;

        /* 拆分多余部分 */
        while (current_order > order) {
            current_order--;
            buddy = page + (1 << current_order);
            list_add(&buddy->lru,
                     &free_area[current_order].free_list);
            free_area[current_order].nr_free++;
        }
        return page;
    }
    return NULL;
}
```

**拆分逻辑**：如果请求 order=0（1 页）但只有 order=2（4 页）的空闲块，需要拆分：
```
4页块 → 分成两个 2页块
         → 一个 2页块再分成两个 1页块
           → 返回一个 1页，另一个放入 free_area[0]
         → 另一个 2页块放入 free_area[1]
```

### free_pages 中的 buddy 合并

```c
void free_pages(struct page *page, unsigned int order) {
    while (order < MAX_ORDER) {
        buddy_pfn = page_to_pfn(page) ^ (1 << order);
        buddy = pfn_to_page(buddy_pfn);

        if (!page_is_free(buddy, order))
            break;

        list_del(&buddy->lru);
        free_area[order].nr_free--;

        /* 合并：取较小地址的页作为新块 */
        if (buddy < page) page = buddy;
        order++;
    }
    list_add(&page->lru, &free_area[order].free_list);
    free_area[order].nr_free++;
}
```

**XOR 求伙伴**：`buddy_pfn = pfn ^ (1 << order)`。这是 buddy 算法的数学精髓——两个伙伴块的页帧号只在第 `order` 位不同。XOR 翻转该位就得到伙伴的地址。

例如 order=2（4 页块），页帧号 0x100 的伙伴是 `0x100 ^ 4 = 0x104`。合并后变成 order=3（8 页块），起始页帧号 0x100。
