/* SPDX-License-Identifier: GPL-2.0 */
/*
 * arm64os/mm/page_alloc.c
 *
 * Buddy 物理内存分配器
 *
 * 参考：mm/page_alloc.c
 *
 * Buddy 系统是 Linux 内核物理内存管理的核心。
 * 它将内存分成不同大小（2^order 个连续页）的块，
 * 通过"伙伴（Buddy）"关系高效合并/分裂内存块，
 * 最大限度减少外部碎片。
 *
 * Phase 2 简化实现：
 *   - 单 Zone（无 DMA/NORMAL/HIGH 区分）
 *   - MAX_ORDER = 11（最大块 2^10 = 1024页 = 4MB）
 *   - struct page 数组存放在内核 BSS 中（由 memblock 分配）
 *   - 不支持 NUMA、大页、压缩等高级特性
 *
 * 核心数据结构：
 *
 *   struct page：每个物理页的描述符
 *   struct free_area：每个 order 的空闲页块链表
 *   struct zone：管理一个内存区域的所有 free_area
 */

#include <linux/types.h>
#include <linux/list.h>
#include <asm/memory.h>

/* 外部提供的打印函数 */
void boot_printk(const char *s);
void boot_printk_hex(unsigned long val);

/* memblock 遍历接口 */
void memblock_for_each_free_region(void (*fn)(phys_addr_t base, phys_addr_t size));
phys_addr_t memblock_alloc(phys_addr_t size, phys_addr_t align);

/*
 * ============================================================
 * 常量定义
 * ============================================================
 */

/*
 * MAX_ORDER：页块的最大 order（不含）
 * order 0 = 1页（4KB），order 10 = 1024页（4MB）
 * 共 11 个 order 级别（0 到 MAX_ORDER-1）
 */
#define MAX_ORDER           11

/* 物理内存总页数（1GB / 4KB = 262144页） */
#define MAX_PAGES           (PHYS_SIZE >> PAGE_SHIFT)

/*
 * ============================================================
 * 数据结构
 * ============================================================
 */

/*
 * struct page - 物理页描述符
 *
 * 每个 4KB 物理页对应一个 struct page（存放在 page_array 中）。
 * 对于 Buddy 分配器，只用到其中几个字段：
 *
 *   lru:     挂接到 free_area.free_list 的链表节点（仅空闲页有效）
 *   order:   该页块的 order（仅每个块的首页有效，分配后为 UINT_MAX）
 *   flags:   页标志位
 *
 * 参考：include/linux/mm_types.h struct page
 */
struct page {
    unsigned long       flags;      /* 页标志（见 PAGE_* 宏）*/
    struct list_head    lru;        /* 空闲链表节点（宿主：free_area）*/
    unsigned int        order;      /* Buddy 块 order（仅块首页有效）*/
    unsigned int        _pad;       /* 对齐填充 */
};

/* page.flags 位定义 */
#define PAGE_FREE           (1UL << 0)  /* 该页当前空闲（在某个 free_list 中）*/
#define PAGE_RESERVED       (1UL << 1)  /* 该页被系统保留，不可分配 */

/*
 * struct free_area - 某个 order 下的空闲页块管理
 *
 * free_list：双向循环链表，挂接所有该 order 的空闲页块（通过 page.lru）
 * nr_free：空闲页块数量（调试/统计用）
 *
 * 参考：include/linux/mmzone.h struct free_area
 */
struct free_area {
    struct list_head    free_list;
    unsigned long       nr_free;
};

/*
 * struct zone - 内存区域管理
 *
 * 包含所有 MAX_ORDER 个 free_area，组成完整的 Buddy 系统。
 * Phase 2 只有一个全局 zone（无 NUMA 分区）。
 *
 * 参考：include/linux/mmzone.h struct zone
 */
struct zone {
    struct free_area    free_area[MAX_ORDER];
    unsigned long       free_pages;         /* 当前空闲页总数（统计）*/
    unsigned long       total_pages;        /* 该 zone 总页数 */
};

/*
 * ============================================================
 * 全局变量
 * ============================================================
 */

/*
 * page_array：物理页描述符数组
 *
 * 索引方式：page_array[PFN]，其中 PFN = phys_to_pfn(PA)
 * 整个数组由 memblock_alloc 在内核末尾分配（大小 = MAX_PAGES × sizeof(struct page)）。
 *
 * 对于 1GB RAM：262144 × 32字节 = 8MB
 */
static struct page *page_array;

/*
 * 全局 zone（Phase 2 单节点、单 zone）
 */
static struct zone main_zone;

/*
 * ============================================================
 * PFN ↔ struct page 转换
 * ============================================================
 */

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

static inline struct page *phys_to_page(phys_addr_t phys)
{
    return pfn_to_page(phys_to_pfn(phys));
}

/*
 * ============================================================
 * Buddy 核心算法
 * ============================================================
 */

/*
 * pfn_buddy - 计算 PFN 在给定 order 下的伙伴（Buddy）PFN
 *
 * 伙伴关系：两个相邻的 2^order 对齐的页块互为伙伴。
 * 计算方式：将 PFN 的第 order 位取反（XOR 2^order）。
 *
 * 例：order=1（2页块），PFN=0 → buddy=2，PFN=2 → buddy=0，
 *                         PFN=4 → buddy=6，PFN=6 → buddy=4。
 */
static inline unsigned long pfn_buddy(unsigned long pfn, unsigned int order)
{
    return pfn ^ (1UL << order);
}

/*
 * page_is_buddy - 判断 buddy 页是否可以与 page 合并
 *
 * 条件（同 Linux 内核简化版）：
 *   1. buddy 在有效的 PFN 范围内
 *   2. buddy 当前空闲（PAGE_FREE 标志）
 *   3. buddy 的 order 与当前相同（确保是同一级别的伙伴）
 */
static inline int page_is_buddy(struct page *page, struct page *buddy,
                                  unsigned int order)
{
    unsigned long buddy_pfn = page_to_pfn(buddy);

    /* 检查 buddy 是否在有效范围 */
    if (buddy_pfn >= main_zone.total_pages)
        return 0;

    /* buddy 必须空闲且同 order */
    if (!(buddy->flags & PAGE_FREE))
        return 0;
    if (buddy->order != order)
        return 0;

    return 1;
}

/*
 * __free_pages_ok - 将页块加入 Buddy 空闲链表（并尝试向上合并）
 *
 * 参考：mm/page_alloc.c __free_one_page()
 *
 * 合并算法：
 *   while order < MAX_ORDER - 1:
 *     计算 buddy PFN
 *     if buddy 空闲且同 order:
 *       从链表摘除 buddy
 *       合并（使用较小 PFN 的页作为合并后块的首页）
 *       order++
 *     else: break
 *   将（可能经多次合并后的）块加入 free_area[order].free_list
 */
static void __free_pages_ok(struct page *page, unsigned int order)
{
    unsigned long pfn = page_to_pfn(page);

    while (order < MAX_ORDER - 1) {
        unsigned long buddy_pfn = pfn_buddy(pfn, order);
        struct page *buddy = pfn_to_page(buddy_pfn);

        if (!page_is_buddy(page, buddy, order))
            break;

        /* 从空闲链表摘除 buddy */
        list_del(&buddy->lru);
        main_zone.free_area[order].nr_free--;
        buddy->flags &= ~PAGE_FREE;

        /* 合并：使用较小 PFN 的页作为合并块的首页 */
        if (buddy_pfn < pfn) {
            page = buddy;
            pfn = buddy_pfn;
        }
        order++;
    }

    /* 将合并后的块加入对应 order 的空闲链表 */
    page->order = order;
    page->flags |= PAGE_FREE;
    list_add(&page->lru, &main_zone.free_area[order].free_list);
    main_zone.free_area[order].nr_free++;
}

/*
 * expand - 将大块（found_order）分裂到目标 order
 *          多余的小块放回空闲链表
 *
 * 参考：mm/page_alloc.c expand()
 *
 * 例：请求 order=1（2页），找到 order=3（8页）：
 *   先放回 [4,8) → order=2 链表
 *   再放回 [2,4) → order=1 链表
 *   返回 [0,2)  → 给调用者
 */
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

        /* 分裂出来的高半部分放回空闲链表 */
        buddy = pfn_to_page(buddy_pfn);
        buddy->order = order;
        buddy->flags |= PAGE_FREE;
        list_add(&buddy->lru, &main_zone.free_area[order].free_list);
        main_zone.free_area[order].nr_free++;
    }
}

/*
 * __rmqueue_smallest - 从 free_area[order..MAX_ORDER-1] 中取出最小满足需求的块
 *
 * 参考：mm/page_alloc.c __rmqueue_smallest()
 *
 * 从目标 order 开始向上查找：
 *   找到第一个非空的 free_area[order']
 *   从链表摘除首个页块
 *   如果 order' > order，调用 expand 分裂多余部分
 *   返回页块首页指针
 */
static struct page *__rmqueue_smallest(unsigned int order)
{
    unsigned int current_order;
    struct free_area *area;
    struct page *page;

    for (current_order = order; current_order < MAX_ORDER; current_order++) {
        area = &main_zone.free_area[current_order];
        if (list_empty(&area->free_list))
            continue;

        /* 取出链表首个页块 */
        page = list_first_entry(&area->free_list, struct page, lru);
        list_del(&page->lru);
        area->nr_free--;
        page->flags &= ~PAGE_FREE;

        /* 若找到的块比请求的大，将多余部分放回 */
        if (current_order > order)
            expand(page, order, current_order);

        page->order = order;
        return page;
    }

    return (struct page *)0;    /* 内存不足 */
}

/*
 * ============================================================
 * 公共分配/释放接口
 * ============================================================
 */

/*
 * alloc_pages - 分配 2^order 个连续物理页
 *
 * 参数：
 *   order: 页块大小的对数（0=1页, 1=2页, ..., 10=1024页）
 *
 * 返回：分配到的首页 struct page 指针，失败返回 NULL。
 *
 * C 原型兼容 Linux 内核（简化，无 gfp_mask）：
 *   struct page *alloc_pages(unsigned int order);
 */
struct page *alloc_pages(unsigned int order)
{
    struct page *page;

    if (order >= MAX_ORDER)
        return (struct page *)0;

    page = __rmqueue_smallest(order);
    if (page) {
        unsigned long count = 1UL << order;
        main_zone.free_pages -= count;
    }

    return page;
}

/*
 * __free_pages - 释放 2^order 个连续物理页回 Buddy
 *
 * 参数：
 *   page:  alloc_pages 返回的首页指针
 *   order: 与分配时相同的 order
 *
 * C 原型：void __free_pages(struct page *page, unsigned int order);
 */
void __free_pages(struct page *page, unsigned int order)
{
    unsigned long count;

    if (!page || order >= MAX_ORDER)
        return;

    count = 1UL << order;
    main_zone.free_pages += count;
    __free_pages_ok(page, order);
}

/*
 * page_address - 获取页对应的虚拟地址（Phase 2 恒等映射下 VA == PA）
 */
void *page_address(struct page *page)
{
    return (void *)(unsigned long)page_to_phys(page);
}

/*
 * ============================================================
 * 初始化
 * ============================================================
 */

/*
 * free_memblock_region - memblock 空闲区域回调：将该区域加入 Buddy
 *
 * 由 memblock_for_each_free_region 调用。
 * 按页对齐切割区域，逐页加入 Buddy（以 order=0 加入，
 * 然后依靠合并算法自动升级到更大的 order）。
 */
static void free_memblock_region(phys_addr_t base, phys_addr_t size)
{
    phys_addr_t end = base + size;
    phys_addr_t pa;

    /* 对齐到页边界 */
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

/*
 * buddy_init - 初始化 Buddy 分配器
 *
 * 操作流程：
 *   1. 通过 memblock_alloc 分配 struct page 数组
 *   2. 初始化 zone（free_area 链表头）
 *   3. 调用 memblock_for_each_free_region 将所有空闲内存加入 Buddy
 *
 * 调用时机：memblock_init() 之后，start_kernel() 中调用。
 */
void buddy_init(void)
{
    phys_addr_t page_array_size;
    phys_addr_t page_array_phys;
    unsigned int i;
    unsigned long pfn;

    /* Step 1: 分配 struct page 数组 */
    page_array_size = MAX_PAGES * sizeof(struct page);
    page_array_phys = memblock_alloc(page_array_size, PAGE_SIZE);

    if (!page_array_phys) {
        boot_printk("[buddy] FATAL: cannot allocate page array\n");
        while (1)
            ;
    }

    /* Phase 2 恒等映射下：虚拟地址 == 物理地址 */
    page_array = (struct page *)(unsigned long)page_array_phys;

    boot_printk("[buddy] page array: ");
    boot_printk_hex(page_array_phys);
    boot_printk(", size=");
    boot_printk_hex(page_array_size);
    boot_printk("\n");

    /* Step 2: 初始化 zone */
    main_zone.total_pages = MAX_PAGES;
    main_zone.free_pages  = 0;
    for (i = 0; i < MAX_ORDER; i++) {
        INIT_LIST_HEAD(&main_zone.free_area[i].free_list);
        main_zone.free_area[i].nr_free = 0;
    }

    /* 将所有 struct page 初始化为保留状态（not free） */
    for (pfn = 0; pfn < MAX_PAGES; pfn++) {
        page_array[pfn].flags = PAGE_RESERVED;
        page_array[pfn].order = 0;
        INIT_LIST_HEAD(&page_array[pfn].lru);
    }

    /* Step 3: 将 memblock 空闲区域转交给 Buddy */
    memblock_for_each_free_region(free_memblock_region);

    boot_printk("[buddy] initialized: ");
    boot_printk_hex(main_zone.free_pages);
    boot_printk(" free pages (");
    boot_printk_hex(main_zone.free_pages << PAGE_SHIFT);
    boot_printk(" bytes)\n");
}

/*
 * ============================================================
 * 测试函数（由 start_kernel 调用验证 Buddy 工作正常）
 * ============================================================
 */

/*
 * test_buddy - 验证 Buddy 分配器基本功能
 *
 * 测试项目：
 *   1. 分配 order=0（4KB）并释放
 *   2. 分配 order=2（16KB）并释放
 *   3. 连续分配/释放，验证内存被回收
 *   4. 打印各 order 的空闲块数量
 *
 * 对应文档 2.5 节验证方法。
 */
void test_buddy(void)
{
    struct page *p1, *p2, *p3;
    unsigned long free_before, free_after;
    unsigned int i;

    boot_printk("[buddy] === test_buddy start ===\n");
    free_before = main_zone.free_pages;

    /* 打印各 order 空闲块数 */
    for (i = 0; i < MAX_ORDER; i++) {
        if (main_zone.free_area[i].nr_free > 0) {
            boot_printk("[buddy]   order ");
            {
                char buf[3] = {'0' + i / 10, '0' + i % 10, '\0'};
                if (i < 10) buf[0] = ' ';
                boot_printk(buf);
            }
            boot_printk(": ");
            boot_printk_hex(main_zone.free_area[i].nr_free);
            boot_printk(" blocks (");
            boot_printk_hex(main_zone.free_area[i].nr_free << (PAGE_SHIFT + i));
            boot_printk(" bytes)\n");
        }
    }

    /* Test 1: 分配 order=0（1页 = 4KB）*/
    p1 = alloc_pages(0);
    if (!p1) {
        boot_printk("[buddy] FAIL: alloc order=0 returned NULL\n");
        return;
    }
    boot_printk("[buddy] alloc order=0: PA=");
    boot_printk_hex(page_to_phys(p1));
    boot_printk("\n");

    /* Test 2: 分配 order=2（4页 = 16KB）*/
    p2 = alloc_pages(2);
    if (!p2) {
        boot_printk("[buddy] FAIL: alloc order=2 returned NULL\n");
        __free_pages(p1, 0);
        return;
    }
    boot_printk("[buddy] alloc order=2: PA=");
    boot_printk_hex(page_to_phys(p2));
    boot_printk("\n");

    /* Test 3: 分配 order=4（16页 = 64KB）*/
    p3 = alloc_pages(4);
    if (!p3) {
        boot_printk("[buddy] FAIL: alloc order=4 returned NULL\n");
    } else {
        boot_printk("[buddy] alloc order=4: PA=");
        boot_printk_hex(page_to_phys(p3));
        boot_printk("\n");
        __free_pages(p3, 4);
        boot_printk("[buddy] free  order=4: OK\n");
    }

    /* 释放（顺序与分配相反，测试合并）*/
    __free_pages(p2, 2);
    boot_printk("[buddy] free  order=2: OK\n");
    __free_pages(p1, 0);
    boot_printk("[buddy] free  order=0: OK\n");

    /* 验证内存已完整回收 */
    free_after = main_zone.free_pages;
    if (free_after == free_before) {
        boot_printk("[buddy] memory fully reclaimed: PASS\n");
    } else {
        boot_printk("[buddy] WARNING: free_before=");
        boot_printk_hex(free_before);
        boot_printk(" free_after=");
        boot_printk_hex(free_after);
        boot_printk("\n");
    }

    boot_printk("[buddy] === test_buddy end ===\n");
}
