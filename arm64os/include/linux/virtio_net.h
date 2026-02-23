/* SPDX-License-Identifier: GPL-2.0 */
/*
 * arm64os/include/linux/virtio_net.h
 *
 * VirtIO 网络设备特性/头部
 *
 * 参考：include/uapi/linux/virtio_net.h
 *
 * Phase 6 实现基础网络设备驱动框架。
 * 完整 TCP/IP 协议栈在 Phase 11 实现。
 */

#ifndef __LINUX_VIRTIO_NET_H
#define __LINUX_VIRTIO_NET_H

#include <linux/types.h>

/* 网络设备特性位 */
#define VIRTIO_NET_F_CSUM           0   /* 主机处理校验和 */
#define VIRTIO_NET_F_GUEST_CSUM     1   /* 客户机处理校验和 */
#define VIRTIO_NET_F_MAC            5   /* 设备有 MAC 地址 */
#define VIRTIO_NET_F_STATUS         16  /* 配置空间中有 status 字段 */
#define VIRTIO_NET_F_MRG_RXBUF     15  /* 驱动可以合并接收缓冲区 */

/* 网络设备状态 */
#define VIRTIO_NET_S_LINK_UP        1   /* 链路已连接 */
#define VIRTIO_NET_S_ANNOUNCE       2   /* 发送免费 ARP 通告 */

/*
 * struct virtio_net_hdr - VirtIO 网络数据包头
 *
 * 每个发送/接收数据包前都有此头部。
 */
struct virtio_net_hdr {
    u8  flags;          /* VIRTIO_NET_HDR_F_* */
    u8  gso_type;       /* GSO 类型 */
    u16 hdr_len;        /* 以太网 + IP + TCP 头总长 */
    u16 gso_size;       /* GSO 段大小 */
    u16 csum_start;     /* 校验和起始偏移 */
    u16 csum_offset;    /* 校验和字段偏移 */
};

#define VIRTIO_NET_HDR_F_NEEDS_CSUM     1   /* 需要计算校验和 */
#define VIRTIO_NET_HDR_GSO_NONE         0   /* 无 GSO */

/*
 * VirtIO 网络设备配置空间
 *
 * 参考：include/uapi/linux/virtio_net.h struct virtio_net_config
 */
struct virtio_net_config {
    u8  mac[6];         /* MAC 地址 */
    u16 status;         /* 链路状态（需 VIRTIO_NET_F_STATUS） */
};

/* 最大以太网帧大小 */
#define VIRTIO_NET_MTU          1514    /* 不含 virtio_net_hdr */

/* 接收缓冲区大小（virtio_net_hdr + 最大以太网帧） */
#define VIRTIO_NET_RX_BUF_SIZE  (sizeof(struct virtio_net_hdr) + VIRTIO_NET_MTU)

/*
 * 网络设备驱动接口
 */

/* 初始化 VirtIO 网络设备 */
int virtio_net_init(void);

/* 测试函数 */
void test_virtio_net(void);

#endif /* __LINUX_VIRTIO_NET_H */
