/* SPDX-License-Identifier: GPL-2.0 */
/*
 * arm64os/include/linux/virtio_blk.h
 *
 * VirtIO 块设备请求格式
 *
 * 参考：include/uapi/linux/virtio_blk.h
 */

#ifndef __LINUX_VIRTIO_BLK_H
#define __LINUX_VIRTIO_BLK_H

#include <linux/types.h>

/* 块设备请求类型 */
#define VIRTIO_BLK_T_IN     0   /* 读（设备→驱动） */
#define VIRTIO_BLK_T_OUT    1   /* 写（驱动→设备） */
#define VIRTIO_BLK_T_FLUSH  4   /* 刷新缓存 */

/* 块设备状态字节值 */
#define VIRTIO_BLK_S_OK     0   /* 成功 */
#define VIRTIO_BLK_S_IOERR  1   /* I/O 错误 */
#define VIRTIO_BLK_S_UNSUPP 2   /* 不支持的请求 */

/* 块设备特性位 */
#define VIRTIO_BLK_F_SIZE_MAX   1   /* 支持最大段大小 */
#define VIRTIO_BLK_F_SEG_MAX    2   /* 支持最大段数量 */
#define VIRTIO_BLK_F_GEOMETRY   4   /* 支持磁盘几何信息 */
#define VIRTIO_BLK_F_RO         5   /* 只读设备 */
#define VIRTIO_BLK_F_BLK_SIZE   6   /* 支持块大小协商 */
#define VIRTIO_BLK_F_FLUSH      9   /* 支持 flush 命令 */

/* 扇区大小 */
#define VIRTIO_BLK_SECTOR_SIZE  512

/*
 * struct virtio_blk_req - 块设备请求头
 *
 * 每次块 I/O 请求由 3 个描述符组成：
 *   desc[0]: virtio_blk_req（请求头，驱动→设备只读）
 *   desc[1]: 数据缓冲区（读: 设备可写; 写: 驱动只读）
 *   desc[2]: status 字节（设备→驱动，1字节，0=成功）
 */
struct virtio_blk_req {
    u32 type;       /* VIRTIO_BLK_T_IN 或 VIRTIO_BLK_T_OUT */
    u32 reserved;   /* 保留，必须为 0 */
    u64 sector;     /* 起始扇区号（512字节/扇区） */
};

/*
 * VirtIO 块设备配置空间（MMIO config 区域偏移 0x100）
 *
 * 参考：include/uapi/linux/virtio_blk.h struct virtio_blk_config
 */
struct virtio_blk_config {
    u64 capacity;       /* 设备容量（扇区数） */
    u32 size_max;       /* 最大段大小 */
    u32 seg_max;        /* 最大段数量 */
    /* ... 更多字段（Phase 6 只使用 capacity） */
};

/*
 * 块设备驱动接口
 */

/* 初始化 VirtIO 块设备 */
int virtio_blk_init(void);

/* 读取扇区 */
int virtio_blk_read(u64 sector, void *buf, u32 len);

/* 写入扇区 */
int virtio_blk_write(u64 sector, const void *buf, u32 len);

/* 测试函数 */
void test_virtio_blk(void);

#endif /* __LINUX_VIRTIO_BLK_H */
