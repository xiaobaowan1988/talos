/* SPDX-License-Identifier: GPL-2.0 */
/*
 * arm64os/drivers/virtio/virtio_mmio.c
 *
 * VirtIO MMIO 传输层
 *
 * 参考：drivers/virtio/virtio_mmio.c
 *
 * Phase 6 实现：
 *   - 扫描 QEMU virt machine 的 32 个 VirtIO MMIO slot
 *   - 完成设备初始化状态机（ACKNOWLEDGE → DRIVER → FEATURES_OK → DRIVER_OK）
 *   - 支持 Version 1 (Legacy) 和 Version 2 (Modern) 设备
 *
 * QEMU virt machine MMIO version 说明：
 *   QEMU 8.x 的 virt machine 默认使用 VirtIO MMIO Version 1 (Legacy)。
 *   Version 1 与 Version 2 的主要差异在于队列设置方式：
 *     V1: 连续 vring 布局，写 QUEUE_PFN = phys / page_size
 *     V2: 独立 desc/avail/used 地址，写 QUEUE_READY
 */

#include <linux/types.h>
#include <linux/io.h>
#include <linux/virtio.h>
#include <linux/virtio_mmio.h>
#include <linux/virtio_ring.h>
#include <asm/memory.h>

/* 外部函数声明 */
void boot_printk(const char *s);
void boot_printk_hex(unsigned long val);

/*
 * virtio_mmio_probe - 探测指定 MMIO slot 的 VirtIO 设备
 *
 * @mmio_base: MMIO 寄存器基地址
 * @irq:       GIC 中断号
 *
 * 返回：0 成功（找到设备），-1 无设备
 */
int virtio_mmio_probe(void *mmio_base, u32 irq)
{
    u32 magic, version, device_id, vendor_id;
    struct virtio_device *dev;

    /* 检查 magic 值 */
    magic = readl(mmio_base + VIRTIO_MMIO_MAGIC_VALUE);
    if (magic != VIRTIO_MMIO_MAGIC)
        return -1;

    /* 读取版本和设备 ID */
    version   = readl(mmio_base + VIRTIO_MMIO_VERSION);
    device_id = readl(mmio_base + VIRTIO_MMIO_DEVICE_ID);
    vendor_id = readl(mmio_base + VIRTIO_MMIO_VENDOR_ID);

    /* device_id 为 0 表示该 slot 未绑定设备 */
    if (device_id == 0)
        return -1;

    if (virtio_device_count >= VIRTIO_MAX_DEVICES)
        return -1;

    boot_printk("[virtio_mmio] found device: id=");
    boot_printk_hex(device_id);
    boot_printk(" vendor=");
    boot_printk_hex(vendor_id);
    boot_printk(" ver=");
    boot_printk_hex(version);
    boot_printk(" at ");
    boot_printk_hex((unsigned long)mmio_base);
    boot_printk(" irq=");
    boot_printk_hex(irq);
    boot_printk("\n");

    /* 注册到全局设备表 */
    dev = &virtio_devices[virtio_device_count++];
    dev->mmio_base  = mmio_base;
    dev->device_id  = device_id;
    dev->vendor_id  = vendor_id;
    dev->version    = version;
    dev->irq        = irq;
    dev->features   = 0;
    dev->status     = 0;
    dev->num_vqs    = 0;

    return 0;
}

/*
 * virtio_mmio_init_device - 完成设备初始化状态机
 *
 * @dev:              VirtIO 设备
 * @driver_features:  驱动支持的特性位（低32位）
 *
 * 返回：0 成功，-1 失败
 *
 * Version 1 (Legacy) 初始化流程：
 *   1. Reset → 2. ACKNOWLEDGE → 3. DRIVER → 4. 特性协商 →
 *   5. 设置 GUEST_PAGE_SIZE → 6. 队列设置 → 7. DRIVER_OK
 *   注：Legacy 模式没有 FEATURES_OK 步骤
 *
 * Version 2 (Modern) 初始化流程：
 *   1. Reset → 2. ACKNOWLEDGE → 3. DRIVER → 4. 特性协商 →
 *   5. FEATURES_OK → 6. 队列设置 → 7. DRIVER_OK
 */
int virtio_mmio_init_device(struct virtio_device *dev, u32 driver_features)
{
    void *base = dev->mmio_base;
    u32 status;
    u32 host_features;

    /* Step 0: Reset — 写 0 重置设备 */
    writel(0, base + VIRTIO_MMIO_STATUS);

    /* Step 1: ACKNOWLEDGE */
    status = VIRTIO_STATUS_ACKNOWLEDGE;
    writel(status, base + VIRTIO_MMIO_STATUS);

    /* Step 2: DRIVER */
    status |= VIRTIO_STATUS_DRIVER;
    writel(status, base + VIRTIO_MMIO_STATUS);

    /* Step 3: 特性协商 — 读取设备特性（低32位） */
    writel(0, base + VIRTIO_MMIO_DEVICE_FEATURES_SEL);
    host_features = readl(base + VIRTIO_MMIO_DEVICE_FEATURES);

    /* 取交集：驱动和设备都支持的特性 */
    dev->features = host_features & driver_features;

    /* 写入驱动接受的特性 */
    writel(0, base + VIRTIO_MMIO_DRIVER_FEATURES_SEL);
    writel(dev->features, base + VIRTIO_MMIO_DRIVER_FEATURES);

    if (dev->version >= 2) {
        /* Version 2: FEATURES_OK step */
        status |= VIRTIO_STATUS_FEATURES_OK;
        writel(status, base + VIRTIO_MMIO_STATUS);

        /* 验证设备接受了特性 */
        if (!(readl(base + VIRTIO_MMIO_STATUS) & VIRTIO_STATUS_FEATURES_OK)) {
            boot_printk("[virtio_mmio] FAIL: device rejected features\n");
            writel(VIRTIO_STATUS_FAILED, base + VIRTIO_MMIO_STATUS);
            return -1;
        }
    } else {
        /*
         * Version 1 (Legacy): 设置 GUEST_PAGE_SIZE
         *
         * Legacy MMIO 需要驱动告知页大小，用于 QUEUE_PFN 计算。
         * 参考：drivers/virtio/virtio_mmio.c: vm_setup_vq() (legacy path)
         */
        writel(PAGE_SIZE, base + VIRTIO_MMIO_GUEST_PAGE_SIZE);
    }

    dev->status = status;
    return 0;
}

/*
 * virtio_mmio_device_ready - 设置 DRIVER_OK，完成设备初始化
 *
 * 在队列设置完成后调用。
 */
void virtio_mmio_device_ready(struct virtio_device *dev)
{
    u32 status = readl(dev->mmio_base + VIRTIO_MMIO_STATUS);
    status |= VIRTIO_STATUS_DRIVER_OK;
    writel(status, dev->mmio_base + VIRTIO_MMIO_STATUS);
    dev->status = status;
}

/*
 * virtio_mmio_setup_vq - 设置指定队列
 *
 * @mmio_base:   MMIO 基地址
 * @queue_index: 队列号
 * @num:         请求的队列大小（0 = 使用设备默认最大值）
 *
 * 返回：已初始化的 virtqueue 指针，失败返回 NULL
 *
 * 此函数根据设备版本选择不同的队列设置路径。
 */
struct virtqueue *virtio_mmio_setup_vq(void *mmio_base, int queue_index,
                                        u16 num)
{
    static struct virtqueue vq_pool[VIRTIO_MAX_DEVICES * VIRTIO_MAX_QUEUES];
    static int vq_pool_idx = 0;

    struct virtqueue *vq;
    u32 max_num;
    u32 version;

    if (vq_pool_idx >= VIRTIO_MAX_DEVICES * VIRTIO_MAX_QUEUES)
        return NULL;

    version = readl(mmio_base + VIRTIO_MMIO_VERSION);

    /* 选择队列 */
    writel(queue_index, mmio_base + VIRTIO_MMIO_QUEUE_SEL);

    /* 读取设备支持的最大队列大小 */
    max_num = readl(mmio_base + VIRTIO_MMIO_QUEUE_NUM_MAX);
    if (max_num == 0) {
        boot_printk("[virtio_mmio] queue not available (max=0)\n");
        return NULL;
    }

    /* 确定实际使用的队列大小 */
    if (num == 0 || num > max_num)
        num = (max_num > 256) ? 256 : (u16)max_num;

    vq = &vq_pool[vq_pool_idx++];

    if (virtqueue_init(vq, mmio_base, (u16)queue_index, num, version) != 0)
        return NULL;

    return vq;
}

/*
 * virtio_mmio_notify - 通知设备有新请求
 */
void virtio_mmio_notify(void *mmio_base, int queue_index)
{
    writel(queue_index, mmio_base + VIRTIO_MMIO_QUEUE_NOTIFY);
}

/*
 * virtio_mmio_read_config - 读取设备配置空间（按字节）
 *
 * @mmio_base: MMIO 基地址
 * @offset:    配置空间内偏移
 * @buf:       目标缓冲区
 * @len:       读取长度
 */
void virtio_mmio_read_config(void *mmio_base, u32 offset,
                              void *buf, u32 len)
{
    u8 *dst = (u8 *)buf;
    u32 i;

    for (i = 0; i < len; i++)
        dst[i] = readb(mmio_base + VIRTIO_MMIO_CONFIG + offset + i);
}
