/* SPDX-License-Identifier: GPL-2.0 */
/*
 * arm64os/include/linux/virtio.h
 *
 * VirtIO 设备/驱动接口
 *
 * 参考：include/linux/virtio.h, include/uapi/linux/virtio_ids.h
 *
 * Phase 6 简化实现：
 *   - 固定数组管理设备（无动态总线注册）
 *   - 设备类型通过 device_id 区分
 */

#ifndef __LINUX_VIRTIO_H
#define __LINUX_VIRTIO_H

#include <linux/types.h>

/* VirtIO 设备类型 ID（参考 include/uapi/linux/virtio_ids.h） */
#define VIRTIO_ID_NET       1   /* virtio net */
#define VIRTIO_ID_BLOCK     2   /* virtio block */
#define VIRTIO_ID_CONSOLE   3   /* virtio console */
#define VIRTIO_ID_RNG       4   /* virtio rng */
#define VIRTIO_ID_9P        9   /* 9p virtio console */

/* 设备状态位（按顺序设置，参考 virtio spec 2.1）*/
#define VIRTIO_STATUS_ACKNOWLEDGE    1   /* 驱动发现了设备 */
#define VIRTIO_STATUS_DRIVER         2   /* 驱动知道如何驱动该设备 */
#define VIRTIO_STATUS_DRIVER_OK      4   /* 驱动完全就绪 */
#define VIRTIO_STATUS_FEATURES_OK    8   /* 特性协商完成 */
#define VIRTIO_STATUS_DEVICE_NEEDS_RESET 64  /* 设备需要重置 */
#define VIRTIO_STATUS_FAILED         128 /* 出现错误 */

/* 最大支持的 VirtIO 设备数（QEMU virt machine 有 32 个 transport slot） */
#define VIRTIO_MAX_DEVICES  32

/* 每个设备最大 virtqueue 数 */
#define VIRTIO_MAX_QUEUES   4

/* 前向声明 */
struct virtqueue;

/*
 * struct virtio_device - VirtIO 设备描述符
 *
 * 参考：include/linux/virtio.h struct virtio_device
 */
struct virtio_device {
    void            *mmio_base;     /* MMIO 寄存器基地址 */
    u32              device_id;     /* 设备类型 ID（1=net, 2=blk） */
    u32              vendor_id;     /* 厂商 ID */
    u32              version;       /* MMIO 版本（1=legacy, 2=modern） */
    u32              irq;           /* GIC 中断号（SPI INTID） */
    u32              features;      /* 协商后的特性位（低32位） */
    u32              status;        /* 当前设备状态 */
    struct virtqueue *vqs[VIRTIO_MAX_QUEUES]; /* 关联的 virtqueue */
    int              num_vqs;       /* virtqueue 数量 */
};

/*
 * 全局设备表
 */
extern struct virtio_device virtio_devices[VIRTIO_MAX_DEVICES];
extern int virtio_device_count;

/*
 * virtio_init - 探测并初始化所有 VirtIO MMIO 设备
 *
 * 扫描 QEMU virt machine 的 32 个 VirtIO MMIO slot，
 * 注册发现的设备到 virtio_devices[] 表中。
 */
void virtio_init(void);

/*
 * virtio_find_device - 查找指定类型的 VirtIO 设备
 *
 * @device_id: 设备类型 ID（如 VIRTIO_ID_BLOCK）
 * 返回：找到的设备指针，未找到返回 NULL
 */
struct virtio_device *virtio_find_device(u32 device_id);

#endif /* __LINUX_VIRTIO_H */
