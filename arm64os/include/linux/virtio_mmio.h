/* SPDX-License-Identifier: GPL-2.0 */
/*
 * arm64os/include/linux/virtio_mmio.h
 *
 * VirtIO MMIO 传输层寄存器定义
 *
 * 参考：include/linux/virtio_mmio.h
 *       VirtIO Specification v1.2, Section 4.2.2 (MMIO Device Register Layout)
 *
 * QEMU virt machine: VirtIO MMIO 基地址 = 0x0a000000，步进 0x200
 * 每个设备占 512 字节的 MMIO 空间
 */

#ifndef __LINUX_VIRTIO_MMIO_H
#define __LINUX_VIRTIO_MMIO_H

/* QEMU virt machine VirtIO MMIO 地址布局 */
#define VIRTIO_MMIO_BASE        0x0a000000UL    /* 第一个 slot 的基地址 */
#define VIRTIO_MMIO_STRIDE      0x200           /* 每个 slot 的 MMIO 空间大小 */
#define VIRTIO_MMIO_NUM_SLOTS   32              /* QEMU virt 支持的 slot 总数 */

/*
 * VirtIO MMIO 寄存器偏移
 *
 * Version 1 (Legacy) 和 Version 2 (Modern) 共用前半部分寄存器，
 * 但队列设置方式不同：
 *   V1: 使用 GUEST_PAGE_SIZE + QUEUE_ALIGN + QUEUE_PFN（连续 vring 布局）
 *   V2: 使用 QUEUE_DESC/AVAIL/USED 独立地址 + QUEUE_READY
 */
#define VIRTIO_MMIO_MAGIC_VALUE         0x000  /* 只读，必须为 0x74726976 ("virt") */
#define VIRTIO_MMIO_VERSION             0x004  /* 只读，版本号（1=legacy, 2=modern）*/
#define VIRTIO_MMIO_DEVICE_ID           0x008  /* 只读，设备类型（1=net, 2=blk）*/
#define VIRTIO_MMIO_VENDOR_ID           0x00c  /* 只读，厂商 ID */
#define VIRTIO_MMIO_DEVICE_FEATURES     0x010  /* 只读，设备支持的特性位 */
#define VIRTIO_MMIO_DEVICE_FEATURES_SEL 0x014  /* 写：选择高32位或低32位特性 */
#define VIRTIO_MMIO_DRIVER_FEATURES     0x020  /* 写：驱动接受的特性位 */
#define VIRTIO_MMIO_DRIVER_FEATURES_SEL 0x024  /* 写：选择高32位或低32位特性 */
#define VIRTIO_MMIO_GUEST_PAGE_SIZE     0x028  /* V1: 写：客户机页大小（必须设置） */
#define VIRTIO_MMIO_QUEUE_SEL           0x030  /* 写：选择操作的队列号 */
#define VIRTIO_MMIO_QUEUE_NUM_MAX       0x034  /* 只读：队列最大容量 */
#define VIRTIO_MMIO_QUEUE_NUM           0x038  /* 写：实际使用的队列大小 */
#define VIRTIO_MMIO_QUEUE_ALIGN         0x03c  /* V1: 写：vring 对齐要求 */
#define VIRTIO_MMIO_QUEUE_PFN           0x040  /* V1: 写：vring 物理页帧号 */
#define VIRTIO_MMIO_QUEUE_READY         0x044  /* V2: 写1表示队列就绪 */
#define VIRTIO_MMIO_QUEUE_NOTIFY        0x050  /* 写：通知设备有新请求 */
#define VIRTIO_MMIO_INTERRUPT_STATUS    0x060  /* 只读：中断原因 */
#define VIRTIO_MMIO_INTERRUPT_ACK       0x064  /* 写：清除中断 */
#define VIRTIO_MMIO_STATUS              0x070  /* 设备状态机 */
/* Version 2 (Modern) 独有寄存器 */
#define VIRTIO_MMIO_QUEUE_DESC_LOW      0x080  /* V2: 描述符表物理地址（低32位）*/
#define VIRTIO_MMIO_QUEUE_DESC_HIGH     0x084  /* V2: 描述符表物理地址（高32位）*/
#define VIRTIO_MMIO_QUEUE_AVAIL_LOW     0x090  /* V2: Available Ring 物理地址 */
#define VIRTIO_MMIO_QUEUE_AVAIL_HIGH    0x094
#define VIRTIO_MMIO_QUEUE_USED_LOW      0x0a0  /* V2: Used Ring 物理地址 */
#define VIRTIO_MMIO_QUEUE_USED_HIGH     0x0a4
#define VIRTIO_MMIO_CONFIG_GENERATION   0x0fc  /* 只读：配置空间 generation counter */
#define VIRTIO_MMIO_CONFIG              0x100  /* 设备特定配置空间起始偏移 */

/* Magic 值 */
#define VIRTIO_MMIO_MAGIC       0x74726976  /* "virt" 的小端序 */

/* MMIO 中断状态位 */
#define VIRTIO_MMIO_INT_VRING   (1 << 0)    /* virtqueue 操作完成 */
#define VIRTIO_MMIO_INT_CONFIG  (1 << 1)    /* 配置空间变更 */

/*
 * VirtIO MMIO 传输层操作函数
 */

/* 探测指定 MMIO slot 的设备 */
int virtio_mmio_probe(void *mmio_base, u32 irq);

/* 设置指定队列 */
struct virtqueue;
struct virtqueue *virtio_mmio_setup_vq(void *mmio_base, int queue_index,
                                        u16 num);

/* 通知设备有新请求 */
void virtio_mmio_notify(void *mmio_base, int queue_index);

#endif /* __LINUX_VIRTIO_MMIO_H */
