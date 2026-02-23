/* SPDX-License-Identifier: GPL-2.0 */
/*
 * arm64os/drivers/block/virtio_blk.c
 *
 * VirtIO 块设备驱动
 *
 * 参考：drivers/block/virtio_blk.c
 *       include/uapi/linux/virtio_blk.h
 *
 * Phase 6 简化实现：
 *   - 单队列（requestq，queue_index = 0）
 *   - 同步 I/O（轮询 Used Ring 等待完成）
 *   - 不支持 multi-segment I/O（每次请求单个数据缓冲区）
 *
 * 一次块请求由 3 个描述符组成：
 *   desc[0] → virtio_blk_req（请求头，驱动→设备只读）
 *   desc[1] → 数据缓冲区（读: 设备可写; 写: 驱动只读）
 *   desc[2] → status 字节（设备→驱动，1字节）
 */

#include <linux/types.h>
#include <linux/io.h>
#include <linux/virtio.h>
#include <linux/virtio_mmio.h>
#include <linux/virtio_ring.h>
#include <linux/virtio_blk.h>
#include <linux/irq.h>
#include <asm/memory.h>

/* 外部函数声明 */
void boot_printk(const char *s);
void boot_printk_hex(unsigned long val);
void gicv3_enable_irq(unsigned int irq);

/* MMIO 设备初始化函数（定义在 virtio_mmio.c） */
int virtio_mmio_init_device(struct virtio_device *dev, u32 driver_features);
void virtio_mmio_device_ready(struct virtio_device *dev);
void virtio_mmio_read_config(void *mmio_base, u32 offset, void *buf, u32 len);

/* 块设备全局状态 */
static struct virtio_device *blk_dev;
static struct virtqueue *blk_vq;
static volatile int blk_irq_pending;

/*
 * virtio_blk_irq_handler - 块设备中断处理函数
 *
 * 由 GIC IRQ handler 调用。
 * 清除中断并设置标志，唤醒轮询循环。
 */
static void virtio_blk_irq_handler(void)
{
    u32 status;

    if (!blk_dev)
        return;

    status = readl(blk_dev->mmio_base + VIRTIO_MMIO_INTERRUPT_STATUS);
    if (status & VIRTIO_MMIO_INT_VRING) {
        writel(status, blk_dev->mmio_base + VIRTIO_MMIO_INTERRUPT_ACK);
        blk_irq_pending = 1;
    }
}

/*
 * virtio_blk_init - 初始化 VirtIO 块设备
 *
 * 查找 VirtIO 块设备，完成特性协商和队列设置。
 *
 * 返回：0 成功，-1 失败
 */
int virtio_blk_init(void)
{
    u64 capacity;

    boot_printk("[virtio_blk] initializing...\n");

    /* 查找块设备 */
    blk_dev = virtio_find_device(VIRTIO_ID_BLOCK);
    if (!blk_dev) {
        boot_printk("[virtio_blk] no block device found\n");
        return -1;
    }

    /* 特性协商（不请求任何高级特性） */
    if (virtio_mmio_init_device(blk_dev, 0) != 0) {
        boot_printk("[virtio_blk] FAIL: device init\n");
        return -1;
    }

    /* 设置 requestq（queue 0） */
    blk_vq = virtio_mmio_setup_vq(blk_dev->mmio_base, 0, 0);
    if (!blk_vq) {
        boot_printk("[virtio_blk] FAIL: setup requestq\n");
        return -1;
    }
    blk_dev->vqs[0] = blk_vq;
    blk_dev->num_vqs = 1;

    /* 设备就绪 */
    virtio_mmio_device_ready(blk_dev);

    /* 读取设备容量 */
    virtio_mmio_read_config(blk_dev->mmio_base, 0, &capacity, sizeof(capacity));
    boot_printk("[virtio_blk] capacity: ");
    boot_printk_hex(capacity);
    boot_printk(" sectors (");
    boot_printk_hex(capacity * VIRTIO_BLK_SECTOR_SIZE);
    boot_printk(" bytes)\n");

    /* 注册中断 */
    request_irq(blk_dev->irq, virtio_blk_irq_handler);
    gicv3_enable_irq(blk_dev->irq);

    boot_printk("[virtio_blk] initialized, irq=");
    boot_printk_hex(blk_dev->irq);
    boot_printk("\n");

    return 0;
}

/*
 * virtio_blk_do_req - 执行一次块 I/O 请求（同步）
 *
 * @type:   VIRTIO_BLK_T_IN（读）或 VIRTIO_BLK_T_OUT（写）
 * @sector: 起始扇区号
 * @buf:    数据缓冲区（必须物理连续）
 * @len:    数据长度（字节，必须为 512 的倍数）
 *
 * 返回：0 成功，-1 失败
 *
 * 参考：drivers/block/virtio_blk.c: virtblk_add_req()
 */
static int virtio_blk_do_req(u32 type, u64 sector, void *buf, u32 len)
{
    struct virtio_blk_req req;
    u8 status_byte = 0xFF;
    u64 sg_addr[3];
    u32 sg_len[3];
    int out_num, in_num;
    int ret;

    if (!blk_dev || !blk_vq)
        return -1;

    req.type     = type;
    req.reserved = 0;
    req.sector   = sector;

    /* 构建散列表：3个描述符 */
    sg_addr[0] = virt_to_phys(&req);
    sg_len[0]  = sizeof(req);

    sg_addr[1] = virt_to_phys(buf);
    sg_len[1]  = len;

    sg_addr[2] = virt_to_phys(&status_byte);
    sg_len[2]  = 1;

    if (type == VIRTIO_BLK_T_IN) {
        /* 读：req(只读) + data(设备可写) + status(设备可写) */
        out_num = 1;
        in_num  = 2;
    } else {
        /* 写：req(只读) + data(只读) + status(设备可写) */
        out_num = 2;
        in_num  = 1;
    }

    /* 提交到 virtqueue */
    ret = virtqueue_add_buf(blk_vq, sg_addr, sg_len, out_num, in_num);
    if (ret < 0) {
        boot_printk("[virtio_blk] FAIL: add_buf\n");
        return -1;
    }

    /* 通知设备 */
    blk_irq_pending = 0;
    virtqueue_kick(blk_vq);

    /* 轮询 Used Ring 等待完成 */
    {
        int timeout = 0;
        while (virtqueue_get_buf(blk_vq, NULL) < 0) {
            cpu_relax();
            timeout++;
            if (timeout > 10000000) {
                boot_printk("[virtio_blk] FAIL: request timeout\n");
                return -1;
            }
        }
    }

    return (status_byte == VIRTIO_BLK_S_OK) ? 0 : -1;
}

/*
 * virtio_blk_read - 读取扇区
 *
 * @sector: 起始扇区号
 * @buf:    目标缓冲区
 * @len:    读取字节数（512的倍数）
 */
int virtio_blk_read(u64 sector, void *buf, u32 len)
{
    return virtio_blk_do_req(VIRTIO_BLK_T_IN, sector, buf, len);
}

/*
 * virtio_blk_write - 写入扇区
 *
 * @sector: 起始扇区号
 * @buf:    源数据缓冲区
 * @len:    写入字节数（512的倍数）
 */
int virtio_blk_write(u64 sector, const void *buf, u32 len)
{
    return virtio_blk_do_req(VIRTIO_BLK_T_OUT, sector, (void *)buf, len);
}

/*
 * test_virtio_blk - 验证 VirtIO 块设备
 *
 * 测试项：
 *   1. 读取第0扇区（512字节）
 *   2. 打印前16字节（十六进制）
 *   3. 写入测试数据到扇区1，回读验证
 */
void test_virtio_blk(void)
{
    /* 使用页对齐缓冲区（避免跨页问题） */
    static u8 __attribute__((aligned(4096))) read_buf[512];
    static u8 __attribute__((aligned(4096))) write_buf[512];
    int ret, i;

    boot_printk("[virtio_blk] === test_virtio_blk start ===\n");

    if (!blk_dev) {
        boot_printk("[virtio_blk] skip: no block device\n");
        boot_printk("[virtio_blk] === test_virtio_blk end ===\n");
        return;
    }

    /* Test 1: 读取扇区 0 */
    boot_printk("[virtio_blk] reading sector 0...\n");
    ret = virtio_blk_read(0, read_buf, 512);
    if (ret != 0) {
        boot_printk("[virtio_blk] FAIL: read sector 0\n");
        boot_printk("[virtio_blk] === test_virtio_blk end ===\n");
        return;
    }

    boot_printk("[virtio_blk] sector 0 data: ");
    for (i = 0; i < 16; i++) {
        static const char hex[] = "0123456789abcdef";
        char h[4];
        h[0] = hex[read_buf[i] >> 4];
        h[1] = hex[read_buf[i] & 0xf];
        h[2] = ' ';
        h[3] = '\0';
        boot_printk(h);
    }
    boot_printk("\n");

    /* Test 2: 写入扇区 1，回读验证 */
    boot_printk("[virtio_blk] write+read test on sector 1...\n");

    /* 填充测试模式 */
    for (i = 0; i < 512; i++)
        write_buf[i] = (u8)(i & 0xFF);

    ret = virtio_blk_write(1, write_buf, 512);
    if (ret != 0) {
        boot_printk("[virtio_blk] FAIL: write sector 1\n");
        boot_printk("[virtio_blk] === test_virtio_blk end ===\n");
        return;
    }

    /* 清除读缓冲区 */
    for (i = 0; i < 512; i++)
        read_buf[i] = 0;

    ret = virtio_blk_read(1, read_buf, 512);
    if (ret != 0) {
        boot_printk("[virtio_blk] FAIL: read sector 1\n");
        boot_printk("[virtio_blk] === test_virtio_blk end ===\n");
        return;
    }

    /* 比较数据 */
    {
        int match = 1;
        for (i = 0; i < 512; i++) {
            if (read_buf[i] != write_buf[i]) {
                match = 0;
                break;
            }
        }
        if (match) {
            boot_printk("[virtio_blk] write+read verify: PASS\n");
        } else {
            boot_printk("[virtio_blk] write+read verify: FAIL at byte ");
            boot_printk_hex(i);
            boot_printk("\n");
        }
    }

    boot_printk("[virtio_blk] VirtIO blk read OK\n");
    boot_printk("[virtio_blk] === test_virtio_blk end ===\n");
}
