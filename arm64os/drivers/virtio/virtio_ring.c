/* SPDX-License-Identifier: GPL-2.0 */
/*
 * arm64os/drivers/virtio/virtio_ring.c
 *
 * virtqueue (vring) 操作实现
 *
 * 参考：drivers/virtio/virtio_ring.c
 *
 * Phase 6 实现：
 *   - 支持 VirtIO MMIO Version 1 (Legacy) 和 Version 2 (Modern)
 *   - Version 1: 连续 vring 布局（desc + avail + padding + used），
 *     通过 QUEUE_PFN 告知设备
 *   - Version 2: 独立 desc/avail/used 地址，通过 QUEUE_READY 激活
 *   - 同步 I/O（轮询 Used Ring，不依赖中断）
 *   - 空闲描述符使用 next 字段组成链表
 */

#include <linux/types.h>
#include <linux/io.h>
#include <linux/virtio_ring.h>
#include <linux/virtio_mmio.h>
#include <asm/memory.h>

/* 外部函数声明 */
void boot_printk(const char *s);
void boot_printk_hex(unsigned long val);

struct page;
struct page *alloc_pages(unsigned int order);
void *page_address(struct page *page);

/*
 * 简易 memset（freestanding 环境无 libc）
 */
static void *vq_memset(void *s, int c, size_t n)
{
    unsigned char *p = s;
    while (n--)
        *p++ = (unsigned char)c;
    return s;
}

/*
 * vring_size_legacy - 计算 Legacy vring 连续布局的总字节数
 *
 * Legacy VirtIO MMIO 要求 desc + avail + padding + used 在一块连续内存中。
 *
 * 布局：
 *   [0]                          : desc table (num * 16 bytes)
 *   [num * 16]                   : avail ring (4 + 2*num + 2 bytes)
 *   [ALIGN(desc+avail, align)]   : used ring  (4 + 8*num + 2 bytes)
 *
 * 参考：include/uapi/linux/virtio_ring.h: vring_size()
 */
static size_t vring_size_legacy(u16 num, u32 align)
{
    size_t desc_avail_size;
    size_t used_size;
    size_t aligned_offset;

    /* desc table + avail ring (包括 used_event) */
    desc_avail_size = (size_t)num * sizeof(struct vring_desc) +
                      sizeof(u16) * (3 + (size_t)num);

    /* 对齐到 align 边界 */
    aligned_offset = (desc_avail_size + align - 1) & ~((size_t)align - 1);

    /* used ring (包括 avail_event) */
    used_size = sizeof(u16) * 3 +
                (size_t)num * sizeof(struct vring_used_elem);

    return aligned_offset + used_size;
}

/*
 * virtqueue_init - 初始化 virtqueue
 *
 * 分配 vring 内存，初始化空闲描述符链，配置 MMIO 寄存器。
 *
 * @vq:          virtqueue 结构体（由调用者提供存储）
 * @mmio_base:   所属设备的 MMIO 基地址
 * @queue_index: 队列号
 * @num:         队列容量（应为2的幂次）
 * @version:     MMIO 版本（1=legacy, 2=modern）
 *
 * 返回：0 成功，-1 失败
 */
int virtqueue_init(struct virtqueue *vq, void *mmio_base,
                   u16 queue_index, u16 num, u32 version)
{
    u16 i;

    /* 选择队列 */
    writel(queue_index, mmio_base + VIRTIO_MMIO_QUEUE_SEL);

    /* 设置队列大小 */
    writel(num, mmio_base + VIRTIO_MMIO_QUEUE_NUM);

    if (version == 1) {
        /*
         * Version 1 (Legacy): 连续 vring 布局
         *
         * 整个 vring（desc + avail + padding + used）必须在一块
         * 连续的页对齐物理内存中。设备通过 QUEUE_PFN 获取基址。
         */
        size_t total_size;
        int order;
        struct page *vring_page;
        void *vring_va;
        u64 vring_pa;
        u32 align = PAGE_SIZE;

        total_size = vring_size_legacy(num, align);

        /* 计算分配 order */
        order = 0;
        while ((PAGE_SIZE << order) < total_size)
            order++;

        vring_page = alloc_pages(order);
        if (!vring_page) {
            boot_printk("[virtio_ring] FAIL: alloc vring pages\n");
            return -1;
        }
        vring_va = page_address(vring_page);
        vq_memset(vring_va, 0, PAGE_SIZE << order);

        vring_pa = virt_to_phys(vring_va);

        /* 设置 vq 结构体字段 */
        vq->desc  = (struct vring_desc *)vring_va;
        vq->avail = (struct vring_avail *)((char *)vring_va +
                     (size_t)num * sizeof(struct vring_desc));

        /* used ring 在对齐边界之后 */
        {
            size_t desc_avail_size = (size_t)num * sizeof(struct vring_desc) +
                                      sizeof(u16) * (3 + (size_t)num);
            size_t used_offset = (desc_avail_size + align - 1) & ~(align - 1);
            vq->used = (struct vring_used *)((char *)vring_va + used_offset);
        }

        /* 设置对齐和 PFN */
        writel(align, mmio_base + VIRTIO_MMIO_QUEUE_ALIGN);
        writel((u32)(vring_pa / PAGE_SIZE),
               mmio_base + VIRTIO_MMIO_QUEUE_PFN);

    } else {
        /*
         * Version 2 (Modern): 独立 desc/avail/used 地址
         */
        struct page *desc_page, *used_page;
        void *desc_va, *used_va;
        u64 desc_pa, avail_pa, used_pa;
        size_t desc_avail_size;
        int order;

        /* 计算 desc + avail 所需大小 */
        desc_avail_size = (size_t)num * sizeof(struct vring_desc) +
                          sizeof(u16) * (3 + (size_t)num);

        order = 0;
        while ((PAGE_SIZE << order) < desc_avail_size)
            order++;

        desc_page = alloc_pages(order);
        if (!desc_page) {
            boot_printk("[virtio_ring] FAIL: alloc desc+avail pages\n");
            return -1;
        }
        desc_va = page_address(desc_page);
        vq_memset(desc_va, 0, PAGE_SIZE << order);

        /* Used Ring */
        {
            size_t used_size = sizeof(u16) * 3 +
                               (size_t)num * sizeof(struct vring_used_elem);
            int used_order = 0;
            while ((PAGE_SIZE << used_order) < used_size)
                used_order++;

            used_page = alloc_pages(used_order);
            if (!used_page) {
                boot_printk("[virtio_ring] FAIL: alloc used ring pages\n");
                return -1;
            }
            used_va = page_address(used_page);
            vq_memset(used_va, 0, PAGE_SIZE << used_order);
        }

        vq->desc  = (struct vring_desc *)desc_va;
        vq->avail = (struct vring_avail *)((char *)desc_va +
                     (size_t)num * sizeof(struct vring_desc));
        vq->used  = (struct vring_used *)used_va;

        /* 告诉设备三张表的物理地址 */
        desc_pa  = virt_to_phys(vq->desc);
        avail_pa = virt_to_phys(vq->avail);
        used_pa  = virt_to_phys(vq->used);

        writel((u32)desc_pa,         mmio_base + VIRTIO_MMIO_QUEUE_DESC_LOW);
        writel((u32)(desc_pa >> 32), mmio_base + VIRTIO_MMIO_QUEUE_DESC_HIGH);

        writel((u32)avail_pa,         mmio_base + VIRTIO_MMIO_QUEUE_AVAIL_LOW);
        writel((u32)(avail_pa >> 32), mmio_base + VIRTIO_MMIO_QUEUE_AVAIL_HIGH);

        writel((u32)used_pa,         mmio_base + VIRTIO_MMIO_QUEUE_USED_LOW);
        writel((u32)(used_pa >> 32), mmio_base + VIRTIO_MMIO_QUEUE_USED_HIGH);

        /* 激活队列 (V2 only) */
        writel(1, mmio_base + VIRTIO_MMIO_QUEUE_READY);
    }

    /* 通用字段初始化 */
    vq->mmio_base    = mmio_base;
    vq->queue_index  = queue_index;
    vq->num          = num;
    vq->free_head    = 0;
    vq->num_free     = num;
    vq->last_used_idx = 0;

    /* 初始化空闲描述符链表 */
    for (i = 0; i < num - 1; i++) {
        vq->desc[i].next = i + 1;
        vq->desc[i].flags = 0;
    }
    vq->desc[num - 1].next = 0xFFFF;
    vq->desc[num - 1].flags = 0;

    boot_printk("[virtio_ring] queue ");
    {
        char qbuf[4];
        qbuf[0] = '0' + queue_index;
        qbuf[1] = '\0';
        boot_printk(qbuf);
    }
    boot_printk(" initialized: num=");
    boot_printk_hex(num);
    boot_printk(" desc=");
    boot_printk_hex(virt_to_phys(vq->desc));
    boot_printk("\n");

    return 0;
}

/*
 * alloc_desc - 从空闲链表分配一个描述符
 *
 * 返回：描述符索引，-1 表示无空闲
 */
static int alloc_desc(struct virtqueue *vq)
{
    u16 idx;

    if (vq->num_free == 0)
        return -1;

    idx = vq->free_head;
    vq->free_head = vq->desc[idx].next;
    vq->num_free--;
    return (int)idx;
}

/*
 * free_desc_chain - 归还描述符链到空闲链表
 *
 * @vq:   virtqueue
 * @head: 链头描述符索引
 */
static void free_desc_chain(struct virtqueue *vq, u16 head)
{
    u16 idx = head;

    for (;;) {
        u16 next = vq->desc[idx].next;
        int has_next = vq->desc[idx].flags & VRING_DESC_F_NEXT;

        /* 归还到空闲链表头部 */
        vq->desc[idx].flags = 0;
        vq->desc[idx].next = vq->free_head;
        vq->free_head = idx;
        vq->num_free++;

        if (!has_next)
            break;
        idx = next;
    }
}

/*
 * virtqueue_add_buf - 向 virtqueue 提交 I/O 缓冲区
 *
 * 参考：drivers/virtio/virtio_ring.c virtqueue_add()
 */
int virtqueue_add_buf(struct virtqueue *vq,
                      u64 *sg_addr, u32 *sg_len,
                      int out_num, int in_num)
{
    int total = out_num + in_num;
    int head_idx;
    int prev_idx = -1;
    int i;

    if (total <= 0 || vq->num_free < (u16)total)
        return -1;

    head_idx = -1;

    for (i = 0; i < total; i++) {
        int idx = alloc_desc(vq);
        if (idx < 0)
            return -1;

        if (head_idx < 0)
            head_idx = idx;

        vq->desc[idx].addr  = sg_addr[i];
        vq->desc[idx].len   = sg_len[i];
        vq->desc[idx].flags = 0;

        /* 设置读写标志 */
        if (i >= out_num)
            vq->desc[idx].flags |= VRING_DESC_F_WRITE;

        /* 链接前一个描述符 */
        if (prev_idx >= 0) {
            vq->desc[prev_idx].flags |= VRING_DESC_F_NEXT;
            vq->desc[prev_idx].next   = (u16)idx;
        }

        prev_idx = idx;
    }

    /* 提交到 Available Ring */
    {
        u16 avail_idx = vq->avail->idx % vq->num;
        vq->avail->ring[avail_idx] = (u16)head_idx;
        wmb();  /* 确保描述符内容对设备可见 */
        vq->avail->idx++;
        wmb();
    }

    return head_idx;
}

/*
 * virtqueue_kick - 通知设备有新请求
 *
 * 参考：drivers/virtio/virtio_ring.c virtqueue_notify()
 */
void virtqueue_kick(struct virtqueue *vq)
{
    mb();
    writel(vq->queue_index, vq->mmio_base + VIRTIO_MMIO_QUEUE_NOTIFY);
}

/*
 * virtqueue_get_buf - 从 Used Ring 获取已完成的请求
 *
 * 参考：drivers/virtio/virtio_ring.c virtqueue_get_buf_ctx()
 */
int virtqueue_get_buf(struct virtqueue *vq, u32 *len_out)
{
    struct vring_used_elem *e;
    u16 used_idx;
    u16 head;

    rmb();

    if (vq->last_used_idx == vq->used->idx)
        return -1;

    used_idx = vq->last_used_idx % vq->num;
    e = &vq->used->ring[used_idx];

    head = (u16)e->id;
    if (len_out)
        *len_out = e->len;

    /* 归还描述符链到空闲链表 */
    free_desc_chain(vq, head);

    vq->last_used_idx++;

    return (int)head;
}
