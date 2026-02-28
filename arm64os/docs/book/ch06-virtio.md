# Phase 6：VirtIO 设备驱动

## 知识来源总览

- **VirtIO 规范 v1.1 (OASIS)**：约 50%（MMIO 寄存器、vring 数据结构、设备初始化状态机）
- **QEMU virt 平台**：约 20%（VirtIO MMIO 基地址 0x0A000000）
- **内存屏障 (ARM)**：约 15%（dmb ish 用于 vring 一致性）
- **Phase 6 文档**：约 15%

## VirtIO MMIO 寄存器

```c
#define VIRTIO_MMIO_BASE        0x0A000000UL
#define VIRTIO_MMIO_MAGIC       (VIRTIO_MMIO_BASE + 0x000)  /* 0x74726976 */
#define VIRTIO_MMIO_VERSION     (VIRTIO_MMIO_BASE + 0x004)
#define VIRTIO_MMIO_DEVICE_ID   (VIRTIO_MMIO_BASE + 0x008)
#define VIRTIO_MMIO_STATUS      (VIRTIO_MMIO_BASE + 0x070)
#define VIRTIO_MMIO_QUEUE_SEL   (VIRTIO_MMIO_BASE + 0x030)
#define VIRTIO_MMIO_QUEUE_NUM   (VIRTIO_MMIO_BASE + 0x038)
#define VIRTIO_MMIO_QUEUE_READY (VIRTIO_MMIO_BASE + 0x044)
#define VIRTIO_MMIO_QUEUE_NOTIFY (VIRTIO_MMIO_BASE + 0x050)
```

**来源：VirtIO spec v1.1, Section 4.2.2 MMIO Device Register Layout**。

魔数 `0x74726976` = ASCII "virt"（小端）。设备探测时读取此值确认是 VirtIO 设备。

## 设备初始化状态机

```c
void virtio_device_init(void) {
    /* 1. 重置设备 */
    writel(0, VIRTIO_MMIO_STATUS);

    /* 2. 设置 ACKNOWLEDGE */
    writel(VIRTIO_STATUS_ACKNOWLEDGE, VIRTIO_MMIO_STATUS);

    /* 3. 设置 DRIVER */
    writel(VIRTIO_STATUS_DRIVER, VIRTIO_MMIO_STATUS);

    /* 4. 协商特性 */
    u32 features = readl(VIRTIO_MMIO_DEVICE_FEATURES);
    features &= driver_supported_features;
    writel(features, VIRTIO_MMIO_DRIVER_FEATURES);

    /* 5. 设置 FEATURES_OK */
    writel(VIRTIO_STATUS_FEATURES_OK, VIRTIO_MMIO_STATUS);

    /* 6. 验证 FEATURES_OK 被接受 */
    if (!(readl(VIRTIO_MMIO_STATUS) & VIRTIO_STATUS_FEATURES_OK))
        goto fail;

    /* 7. 配置 virtqueue */
    setup_virtqueues();

    /* 8. 设置 DRIVER_OK */
    writel(VIRTIO_STATUS_DRIVER_OK, VIRTIO_MMIO_STATUS);
}
```

**来源：VirtIO spec Section 3.1 Device Initialization**。

状态机：`RESET → ACKNOWLEDGE → DRIVER → FEATURES_OK → DRIVER_OK`。每步不可跳过——如果设备发现驱动跳过了某步，可能拒绝工作。这是 VirtIO 的握手协议。

## Virtqueue (vring) 数据结构

```c
struct vring_desc {
    __le64 addr;     /* 数据缓冲区的物理地址 */
    __le32 len;      /* 缓冲区长度 */
    __le16 flags;    /* NEXT, WRITE, INDIRECT */
    __le16 next;     /* 下一个描述符索引（链表）*/
};

struct vring_avail {
    __le16 flags;
    __le16 idx;      /* 驱动写的下一个位置 */
    __le16 ring[];   /* 可用描述符索引数组 */
};

struct vring_used {
    __le16 flags;
    __le16 idx;      /* 设备写的下一个位置 */
    struct vring_used_elem ring[];
};
```

**来源：VirtIO spec Section 2.6 Virtqueues**。

vring 是驱动和设备共享的环形缓冲区：
- **desc 数组**：描述数据缓冲区的位置和大小
- **avail 环**：驱动告诉设备"这些描述符已准备好"
- **used 环**：设备告诉驱动"这些描述符已处理完"

## VirtIO-blk 读操作

```c
int virtio_blk_read(u64 sector, void *buf, u32 count) {
    /* 3-descriptor chain: header → data → status */

    /* desc[0]: 请求头（设备读取）*/
    desc[0].addr = __pa(&req_header);
    desc[0].len  = sizeof(struct virtio_blk_req);
    desc[0].flags = VRING_DESC_F_NEXT;
    desc[0].next = 1;

    /* desc[1]: 数据缓冲区（设备写入）*/
    desc[1].addr = __pa(buf);
    desc[1].len  = count * 512;
    desc[1].flags = VRING_DESC_F_NEXT | VRING_DESC_F_WRITE;
    desc[1].next = 2;

    /* desc[2]: 状态字节（设备写入）*/
    desc[2].addr = __pa(&status);
    desc[2].len  = 1;
    desc[2].flags = VRING_DESC_F_WRITE;
    desc[2].next = 0;

    /* 放入 avail 环 */
    avail->ring[avail->idx % queue_size] = 0;
    dmb(ish);                              /* 内存屏障 */
    avail->idx++;
    writel(0, VIRTIO_MMIO_QUEUE_NOTIFY);   /* 通知设备 */
}
```

**3-descriptor chain 的设计**：
- desc[0]：驱动→设备（"我要读扇区 N"）
- desc[1]：设备→驱动（"这是数据"）—— `VRING_DESC_F_WRITE` 表示设备写此缓冲区
- desc[2]：设备→驱动（"操作成功/失败"）

**`dmb ish`（Data Memory Barrier, Inner Shareable）**：确保 avail ring 的写入在 idx 更新之前对设备可见。如果没有屏障，设备可能先看到新的 idx 但看到旧的 ring 内容。
