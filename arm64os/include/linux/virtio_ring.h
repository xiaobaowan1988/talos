/* SPDX-License-Identifier: GPL-2.0 */
/*
 * arm64os/include/linux/virtio_ring.h
 *
 * VirtIO vring 数据结构与 virtqueue 操作接口
 *
 * 参考：include/uapi/linux/virtio_ring.h
 *       include/linux/virtio.h (struct virtqueue)
 *       drivers/virtio/virtio_ring.c
 *
 * vring 由三部分组成：
 *   1. Descriptor Table（描述符表）— 描述 I/O 缓冲区
 *   2. Available Ring（驱动→设备）— 驱动提交的描述符链头索引
 *   3. Used Ring（设备→驱动）— 设备已处理完的描述符链头索引
 */

#ifndef __LINUX_VIRTIO_RING_H
#define __LINUX_VIRTIO_RING_H

#include <linux/types.h>

/* vring 描述符标志位 */
#define VRING_DESC_F_NEXT       1   /* 该描述符有下一个（链式描述符） */
#define VRING_DESC_F_WRITE      2   /* 缓冲区由设备写入（设备→驱动） */
#define VRING_DESC_F_INDIRECT   4   /* 间接描述符（Phase 6 不使用） */

/* Available Ring 标志 */
#define VRING_AVAIL_F_NO_INTERRUPT  1   /* 抑制中断通知 */

/* Used Ring 标志 */
#define VRING_USED_F_NO_NOTIFY      1   /* 抑制设备通知 */

/*
 * struct vring_desc - vring 描述符
 *
 * 每个描述符描述一个 I/O 缓冲区段。
 * 多个描述符通过 next 字段链成一个描述符链，描述一次完整 I/O。
 */
struct vring_desc {
    u64 addr;       /* 缓冲区物理地址 */
    u32 len;        /* 缓冲区长度（字节） */
    u16 flags;      /* VRING_DESC_F_* 标志位 */
    u16 next;       /* 下一个描述符索引（flags & NEXT 时有效）*/
};

/*
 * struct vring_avail - Available Ring（驱动→设备）
 *
 * 驱动将就绪的描述符链头索引写入 ring[]，然后递增 idx。
 */
struct vring_avail {
    u16 flags;      /* VRING_AVAIL_F_NO_INTERRUPT */
    u16 idx;        /* 下一个可用槽的索引（单调递增） */
    u16 ring[];     /* 描述符链头的索引数组，长度 = queue_num */
};

/*
 * struct vring_used_elem - Used Ring 元素
 */
struct vring_used_elem {
    u32 id;         /* 已完成的描述符链头索引 */
    u32 len;        /* 设备实际写入的字节数 */
};

/*
 * struct vring_used - Used Ring（设备→驱动）
 *
 * 设备将已处理完的描述符链头索引写入 ring[]，然后递增 idx。
 */
struct vring_used {
    u16 flags;      /* VRING_USED_F_NO_NOTIFY */
    u16 idx;        /* 下一个已用槽的索引（单调递增） */
    struct vring_used_elem ring[];  /* 长度 = queue_num */
};

/*
 * struct virtqueue - virtqueue 管理结构
 *
 * 参考：drivers/virtio/virtio_ring.c struct vring_virtqueue
 */
struct virtqueue {
    void            *mmio_base;     /* 所属设备的 MMIO 基地址 */
    u16              queue_index;   /* 队列号（0, 1, ...） */
    u16              num;           /* 队列容量（2的幂次） */

    /* 三张表的虚拟地址 */
    struct vring_desc  *desc;
    struct vring_avail *avail;
    struct vring_used  *used;

    /* 空闲描述符链表 */
    u16              free_head;     /* 空闲链表头 */
    u16              num_free;      /* 空闲描述符数量 */

    /* 上次处理的 used->idx */
    u16              last_used_idx;
};

/*
 * virtqueue 操作函数
 */

/*
 * 初始化 virtqueue（分配 vring 内存，初始化描述符链）
 *
 * @version: MMIO 版本（1=legacy 连续布局, 2=modern 独立地址）
 */
int virtqueue_init(struct virtqueue *vq, void *mmio_base,
                   u16 queue_index, u16 num, u32 version);

/*
 * virtqueue_add_buf - 向 virtqueue 提交一次 I/O 请求
 *
 * @vq:        virtqueue
 * @sg_addr:   缓冲区物理地址数组
 * @sg_len:    缓冲区长度数组
 * @out_num:   驱动→设备（只读）缓冲区数量
 * @in_num:    设备→驱动（可写）缓冲区数量
 *
 * 返回：描述符链头索引（成功），-1（失败，空闲描述符不足）
 *
 * 缓冲区排列：sg[0..out_num-1] 为只读，sg[out_num..out_num+in_num-1] 为可写
 */
int virtqueue_add_buf(struct virtqueue *vq,
                      u64 *sg_addr, u32 *sg_len,
                      int out_num, int in_num);

/* 通知设备有新请求（写 QUEUE_NOTIFY 寄存器） */
void virtqueue_kick(struct virtqueue *vq);

/*
 * virtqueue_get_buf - 从 Used Ring 取出已完成的请求
 *
 * @vq:      virtqueue
 * @len_out: 输出：设备写入的字节数（可为 NULL）
 *
 * 返回：描述符链头索引（成功），-1（无已完成请求）
 *
 * 同时将该描述符链归还到空闲链表。
 */
int virtqueue_get_buf(struct virtqueue *vq, u32 *len_out);

#endif /* __LINUX_VIRTIO_RING_H */
