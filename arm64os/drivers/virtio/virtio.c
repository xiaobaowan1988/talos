/* SPDX-License-Identifier: GPL-2.0 */
/*
 * arm64os/drivers/virtio/virtio.c
 *
 * VirtIO 总线框架
 *
 * 参考：drivers/virtio/virtio.c
 *
 * Phase 6 简化实现：
 *   - 全局 virtio_devices[] 数组管理设备
 *   - 扫描所有 VirtIO MMIO slot 并调用 probe
 *   - 按设备类型查找设备
 */

#include <linux/types.h>
#include <linux/io.h>
#include <linux/virtio.h>
#include <linux/virtio_mmio.h>

/* 外部函数声明 */
void boot_printk(const char *s);
void boot_printk_hex(unsigned long val);

/* 全局设备表 */
struct virtio_device virtio_devices[VIRTIO_MAX_DEVICES];
int virtio_device_count = 0;

/*
 * virtio_init - 探测并注册所有 VirtIO MMIO 设备
 *
 * 扫描 QEMU virt machine 的 32 个 VirtIO MMIO slot。
 * 每个 slot 占 0x200 字节，基址 0x0a000000。
 *
 * QEMU virt machine IRQ 映射：
 *   slot i → SPI (16 + i) → GIC INTID (48 + i)
 *
 * 参考：QEMU hw/arm/virt.c: create_virtio_devices()
 */
void virtio_init(void)
{
    int i;

    boot_printk("[virtio] scanning VirtIO MMIO bus...\n");

    for (i = 0; i < VIRTIO_MMIO_NUM_SLOTS; i++) {
        void *mmio_base = (void *)(VIRTIO_MMIO_BASE +
                                    (unsigned long)i * VIRTIO_MMIO_STRIDE);
        u32 irq = 48 + i;  /* SPI 16+i = GIC INTID 48+i */

        virtio_mmio_probe(mmio_base, irq);
    }

    boot_printk("[virtio] found ");
    {
        char buf[4];
        buf[0] = '0' + virtio_device_count;
        buf[1] = '\0';
        boot_printk(buf);
    }
    boot_printk(" device(s)\n");
}

/*
 * virtio_find_device - 查找指定类型的 VirtIO 设备
 *
 * @device_id: 设备类型（VIRTIO_ID_BLOCK, VIRTIO_ID_NET 等）
 * 返回：设备指针，未找到返回 NULL
 */
struct virtio_device *virtio_find_device(u32 device_id)
{
    int i;

    for (i = 0; i < virtio_device_count; i++) {
        if (virtio_devices[i].device_id == device_id)
            return &virtio_devices[i];
    }

    return NULL;
}
