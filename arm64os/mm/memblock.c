/* SPDX-License-Identifier: GPL-2.0 */
/*
 * arm64os/mm/memblock.c
 *
 * 早期物理内存分配器（memblock）
 *
 * 参考：mm/memblock.c
 *
 * memblock 是内核启动早期（Buddy分配器初始化之前）使用的简单内存分配器。
 * 它维护两个区域列表：
 *   memory：所有可用物理内存区域
 *   reserved：已被占用的区域（内核镜像、页表、设备树等）
 *
 * 分配算法（简化版）：
 *   从 memory 区域中找到足够大且未被 reserved 覆盖的空闲区间，
 *   将其加入 reserved 列表并返回起始地址。
 *
 * Phase 2 限制：
 *   - 最多 MEMBLOCK_MAX_REGIONS 个 memory/reserved 区域（静态数组）
 *   - 不支持 NUMA（单节点）
 *   - 不支持区域合并（简化实现）
 */

#include <linux/types.h>
#include <asm/memory.h>

/* 外部提供的打印函数（printk.c） */
void boot_printk(const char *s);
void boot_printk_hex(unsigned long val);

/*
 * ============================================================
 * 数据结构
 * ============================================================
 */

#define MEMBLOCK_MAX_REGIONS    16

/*
 * struct memblock_region - 描述一段连续物理内存区域
 */
struct memblock_region {
    phys_addr_t base;   /* 起始物理地址（页对齐）*/
    phys_addr_t size;   /* 区域大小（字节，页对齐）*/
};

/*
 * struct memblock_type - 同类型区域的集合
 */
struct memblock_type {
    unsigned int        cnt;                            /* 当前区域数量 */
    struct memblock_region  regions[MEMBLOCK_MAX_REGIONS];
};

/*
 * struct memblock - 全局 memblock 状态
 */
static struct memblock {
    struct memblock_type    memory;     /* 所有可用物理内存 */
    struct memblock_type    reserved;   /* 已保留/分配的内存 */
} memblock_data;

/*
 * ============================================================
 * 内部辅助函数
 * ============================================================
 */

/*
 * region_add - 向区域列表末尾追加一个区域（内部辅助）
 */
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

/*
 * regions_overlap - 判断两个区间是否有重叠
 * [base1, base1+size1) 与 [base2, base2+size2) 是否相交
 */
static int regions_overlap(phys_addr_t base1, phys_addr_t size1,
                            phys_addr_t base2, phys_addr_t size2)
{
    return (base1 < base2 + size2) && (base2 < base1 + size1);
}

/*
 * ============================================================
 * 公共接口
 * ============================================================
 */

/*
 * memblock_add - 添加可用物理内存区域
 *
 * 通常由平台初始化代码调用，描述可用的 RAM 范围。
 * Phase 2 中 start_kernel 调用一次，描述 QEMU 的 1GB RAM。
 */
int memblock_add(phys_addr_t base, phys_addr_t size)
{
    return region_add(&memblock_data.memory, base, size);
}

/*
 * memblock_reserve - 标记一段物理内存为已保留
 *
 * 保留的内存不会被 memblock_alloc 分配给新用途。
 * 常见用途：
 *   - 内核镜像（_text 到 _end）
 *   - 设备树（FDT）
 *   - 早期页表
 */
int memblock_reserve(phys_addr_t base, phys_addr_t size)
{
    return region_add(&memblock_data.reserved, base, size);
}

/*
 * memblock_alloc - 从可用内存中分配
 *
 * 参数：
 *   size:  请求的字节数（向上对齐到 PAGE_SIZE）
 *   align: 对齐要求（必须是2的幂，最小 PAGE_SIZE）
 *
 * 返回：分配到的物理地址，失败返回 0。
 *
 * 分配策略（Bottom-Up，从低地址向高地址搜索）：
 *   遍历 memory 中每个区域，找到在该区域内满足对齐且不与
 *   任何 reserved 区域重叠的空闲段，标记为 reserved 并返回。
 */
phys_addr_t memblock_alloc(phys_addr_t size, phys_addr_t align)
{
    struct memblock_type *mem = &memblock_data.memory;
    struct memblock_type *res = &memblock_data.reserved;
    unsigned int i;

    /* 对齐 size 到页边界 */
    if (align < PAGE_SIZE)
        align = PAGE_SIZE;
    size = (size + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);

    /* 遍历每个可用内存区域 */
    for (i = 0; i < mem->cnt; i++) {
        phys_addr_t region_base = mem->regions[i].base;
        phys_addr_t region_end  = region_base + mem->regions[i].size;
        phys_addr_t candidate;
        unsigned int j;
        int conflict;

        /* 在区域内从低地址开始，对齐到 align 边界 */
        candidate = (region_base + align - 1) & ~(align - 1);

        /* 尝试在此区域内找一个合适的位置 */
        while (candidate + size <= region_end) {
            /* 检查候选区间 [candidate, candidate+size) 是否与所有保留区冲突 */
            conflict = 0;
            for (j = 0; j < res->cnt; j++) {
                if (regions_overlap(candidate, size,
                                    res->regions[j].base,
                                    res->regions[j].size)) {
                    /* 冲突：跳过保留区，从保留区末尾继续搜索 */
                    candidate = res->regions[j].base + res->regions[j].size;
                    /* 重新对齐 */
                    candidate = (candidate + align - 1) & ~(align - 1);
                    conflict = 1;
                    break;
                }
            }
            if (!conflict) {
                /* 找到合适位置，标记为已保留并返回 */
                memblock_reserve(candidate, size);
                return candidate;
            }
        }
    }

    boot_printk("[memblock] ERROR: allocation failed, size=");
    boot_printk_hex(size);
    boot_printk("\n");
    return 0;
}

/*
 * memblock_free_region_iter - 遍历所有空闲（未保留）内存区域的回调
 *
 * 参数：
 *   fn:  对每个空闲区域调用的函数，参数为 (base, size)
 *
 * 用于 Buddy 分配器初始化：将所有 memblock 空闲内存传递给 Buddy。
 */
void memblock_for_each_free_region(void (*fn)(phys_addr_t base, phys_addr_t size))
{
    struct memblock_type *mem = &memblock_data.memory;
    struct memblock_type *res = &memblock_data.reserved;
    unsigned int i, j;

    /* 遍历 memory 中每个区域 */
    for (i = 0; i < mem->cnt; i++) {
        phys_addr_t region_base = mem->regions[i].base;
        phys_addr_t region_size = mem->regions[i].size;
        phys_addr_t region_end  = region_base + region_size;

        /*
         * 在 [region_base, region_end) 内，找出所有未被 reserved 覆盖的子段。
         *
         * 简化算法：线性扫描
         *   cursor 从 region_base 开始，遇到 reserved 区就跳过，
         *   把空闲段 [cursor, reserved_base) 传给回调。
         */
        phys_addr_t cursor = region_base;

        /*
         * 对 reserved 区域按 base 做简单排序（冒泡），
         * 确保线性扫描正确性。
         * 注意：这是一个简化实现，效率低但在初始化期间可接受。
         */
        /* 先收集与当前 memory 区域重叠的 reserved 区并排序 */
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

        /* 简单冒泡排序（按 base 升序） */
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

        /* 线性扫描，提取空闲段 */
        cursor = region_base;
        for (k = 0; k < noverlap; k++) {
            if (cursor < overlap[k].base) {
                /* [cursor, overlap[k].base) 是空闲的 */
                fn(cursor, overlap[k].base - cursor);
            }
            if (overlap[k].end > cursor)
                cursor = overlap[k].end;
        }
        /* 最后一段（最后一个reserved之后到region_end） */
        if (cursor < region_end)
            fn(cursor, region_end - cursor);
    }
}

/*
 * memblock_init - 初始化 memblock（由 start_kernel 调用）
 *
 * 参数：
 *   phys_start: 物理内存起始地址（PHYS_OFFSET）
 *   phys_size:  物理内存总大小（PHYS_SIZE）
 *
 * 操作：
 *   1. 注册整个 RAM 为 memory 区域
 *   2. 保留内核镜像（_text 到 _end 按页对齐）
 *   3. 保留 QEMU 用于 FDT/设备等的低内存（0x40000000-0x40080000）
 */
void memblock_init(phys_addr_t phys_start, phys_addr_t phys_size)
{
    /* 由链接脚本定义 */
    extern char _text[];
    extern char _end[];

    phys_addr_t kernel_start = (phys_addr_t)(unsigned long)_text;
    phys_addr_t kernel_end   = (phys_addr_t)(unsigned long)_end;

    /* 向上对齐到页边界 */
    kernel_end = (kernel_end + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);

    /* 1. 注册整个物理 RAM */
    memblock_add(phys_start, phys_size);

    /* 2. 保留内核镜像所在的内存区域 */
    memblock_reserve(kernel_start, kernel_end - kernel_start);

    /*
     * 3. 保留 QEMU virt 低端内存（0x40000000 到内核加载地址之间）
     * 这段包含 QEMU 放置的设备树（FDT）和其他固件数据。
     * 0x40000000 到 0x40080000 = 512KB
     */
    if (kernel_start > phys_start)
        memblock_reserve(phys_start, kernel_start - phys_start);

    boot_printk("[memblock] initialized: RAM ");
    boot_printk_hex(phys_start);
    boot_printk(" - ");
    boot_printk_hex(phys_start + phys_size);
    boot_printk("\n");
    boot_printk("[memblock] kernel reserved: ");
    boot_printk_hex(kernel_start);
    boot_printk(" - ");
    boot_printk_hex(kernel_end);
    boot_printk("\n");
}
