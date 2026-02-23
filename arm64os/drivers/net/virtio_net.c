/* SPDX-License-Identifier: GPL-2.0 */
/*
 * arm64os/drivers/net/virtio_net.c
 *
 * VirtIO 网络设备驱动
 *
 * 参考：drivers/net/virtio_net.c
 *       include/uapi/linux/virtio_net.h
 *
 * Phase 6 简化实现：
 *   - 两个队列：receiveq（0）和 transmitq（1）
 *   - 基础初始化和 MAC 地址读取
 *   - 不实现完整网络收发（Phase 11 TCP/IP 阶段完成）
 */

#include <linux/types.h>
#include <linux/io.h>
#include <linux/virtio.h>
#include <linux/virtio_mmio.h>
#include <linux/virtio_ring.h>
#include <linux/virtio_net.h>
#include <linux/irq.h>
#include <asm/memory.h>

/* 外部函数声明 */
void boot_printk(const char *s);
void boot_printk_hex(unsigned long val);
void gicv3_enable_irq(unsigned int irq);

/* MMIO 设备初始化函数 */
int virtio_mmio_init_device(struct virtio_device *dev, u32 driver_features);
void virtio_mmio_device_ready(struct virtio_device *dev);
void virtio_mmio_read_config(void *mmio_base, u32 offset, void *buf, u32 len);

/* 网络设备全局状态 */
static struct virtio_device *net_dev;
static struct virtqueue *net_rxq;   /* receiveq (queue 0) */
static struct virtqueue *net_txq;   /* transmitq (queue 1) */
static u8 net_mac[6];

/*
 * virtio_net_irq_handler - 网络设备中断处理函数
 */
static void virtio_net_irq_handler(void)
{
    u32 status;

    if (!net_dev)
        return;

    status = readl(net_dev->mmio_base + VIRTIO_MMIO_INTERRUPT_STATUS);
    if (status) {
        writel(status, net_dev->mmio_base + VIRTIO_MMIO_INTERRUPT_ACK);
    }
}

/*
 * virtio_net_init - 初始化 VirtIO 网络设备
 *
 * 返回：0 成功，-1 失败
 */
int virtio_net_init(void)
{
    u32 driver_features;

    boot_printk("[virtio_net] initializing...\n");

    /* 查找网络设备 */
    net_dev = virtio_find_device(VIRTIO_ID_NET);
    if (!net_dev) {
        boot_printk("[virtio_net] no network device found\n");
        return -1;
    }

    /* 请求 MAC 地址特性 */
    driver_features = (1U << VIRTIO_NET_F_MAC);

    /* 特性协商 */
    if (virtio_mmio_init_device(net_dev, driver_features) != 0) {
        boot_printk("[virtio_net] FAIL: device init\n");
        return -1;
    }

    /* 设置 receiveq (queue 0) */
    net_rxq = virtio_mmio_setup_vq(net_dev->mmio_base, 0, 0);
    if (!net_rxq) {
        boot_printk("[virtio_net] FAIL: setup receiveq\n");
        return -1;
    }
    net_dev->vqs[0] = net_rxq;

    /* 设置 transmitq (queue 1) */
    net_txq = virtio_mmio_setup_vq(net_dev->mmio_base, 1, 0);
    if (!net_txq) {
        boot_printk("[virtio_net] FAIL: setup transmitq\n");
        return -1;
    }
    net_dev->vqs[1] = net_txq;
    net_dev->num_vqs = 2;

    /* 设备就绪 */
    virtio_mmio_device_ready(net_dev);

    /* 读取 MAC 地址 */
    virtio_mmio_read_config(net_dev->mmio_base, 0, net_mac, 6);

    boot_printk("[virtio_net] MAC: ");
    {
        static const char hex[] = "0123456789abcdef";
        int i;
        for (i = 0; i < 6; i++) {
            char m[4];
            m[0] = hex[net_mac[i] >> 4];
            m[1] = hex[net_mac[i] & 0xf];
            m[2] = (i < 5) ? ':' : '\0';
            m[3] = '\0';
            boot_printk(m);
        }
    }
    boot_printk("\n");

    /* 注册中断 */
    request_irq(net_dev->irq, virtio_net_irq_handler);
    gicv3_enable_irq(net_dev->irq);

    boot_printk("[virtio_net] initialized, irq=");
    boot_printk_hex(net_dev->irq);
    boot_printk("\n");

    return 0;
}

/*
 * test_virtio_net - 验证 VirtIO 网络设备
 *
 * Phase 6 仅验证初始化成功和 MAC 地址可读。
 * 完整收发测试在 Phase 11（TCP/IP）中实现。
 */
void test_virtio_net(void)
{
    boot_printk("[virtio_net] === test_virtio_net start ===\n");

    if (!net_dev) {
        boot_printk("[virtio_net] skip: no network device\n");
        boot_printk("[virtio_net] === test_virtio_net end ===\n");
        return;
    }

    /* 验证 MAC 地址非全零 */
    {
        int all_zero = 1;
        int i;
        for (i = 0; i < 6; i++) {
            if (net_mac[i] != 0) {
                all_zero = 0;
                break;
            }
        }
        if (!all_zero) {
            boot_printk("[virtio_net] MAC address valid: PASS\n");
        } else {
            boot_printk("[virtio_net] MAC address all zero: FAIL\n");
        }
    }

    /* 验证设备状态为 DRIVER_OK */
    {
        u32 status = readl(net_dev->mmio_base + VIRTIO_MMIO_STATUS);
        if (status & VIRTIO_STATUS_DRIVER_OK) {
            boot_printk("[virtio_net] device status DRIVER_OK: PASS\n");
        } else {
            boot_printk("[virtio_net] device status: ");
            boot_printk_hex(status);
            boot_printk(" FAIL\n");
        }
    }

    boot_printk("[virtio_net] VirtIO net init OK\n");
    boot_printk("[virtio_net] === test_virtio_net end ===\n");
}
