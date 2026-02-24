# Phase 6 Walkthrough: VirtIO 驱动框架 — MMIO 传输层 + 块设备 + 网络设备

> **目标**：让内核访问虚拟磁盘和虚拟网卡。
> **最终效果**：读写块设备成功，网络设备初始化并获取 MAC 地址。

---

## 6.1 为什么用 VirtIO？

真实硬件驱动太复杂（一个 Intel e1000 网卡驱动几千行）。VirtIO 是为虚拟化专门设计的标准接口：

- **统一的传输层** — 所有设备共享同一套环形缓冲区（vring）
- **简单的设备发现** — MMIO 寄存器固定在已知地址
- **高效** — 批量传输，减少虚拟机退出

```
  ┌─────────────┐        ┌────────────────┐
  │  Guest OS   │        │    QEMU        │
  │  (我们的内核)│ ═vring═│  设备后端      │
  │  驱动前端   │        │  (磁盘/网络)   │
  └─────────────┘        └────────────────┘
        │                       │
     共享内存               模拟真实 I/O
     (描述符表)
```

---

## 6.2 QEMU virt 机器的 VirtIO 布局

QEMU 为每个 VirtIO 设备分配一个 MMIO 地址槽：

```
  基地址: 0x0a000000
  步长:   0x200 (512 字节/设备)
  最多:   32 个设备

  Slot 0: 0x0a000000 — VirtIO 设备 0
  Slot 1: 0x0a000200 — VirtIO 设备 1
  ...
  Slot 31: 0x0a003e00 — VirtIO 设备 31
```

---

## 6.3 VirtIO MMIO 寄存器

每个设备有一组标准 MMIO 寄存器：

```c
#define VIRTIO_MMIO_MAGIC_VALUE      0x000  /* 魔数: 0x74726976 = "virt" */
#define VIRTIO_MMIO_VERSION          0x004  /* 版本: 1=Legacy, 2=Modern */
#define VIRTIO_MMIO_DEVICE_ID        0x008  /* 设备类型 */
#define VIRTIO_MMIO_VENDOR_ID        0x00c  /* 厂商 ID */
#define VIRTIO_MMIO_DEVICE_FEATURES  0x010  /* 设备支持的特性 */
#define VIRTIO_MMIO_DRIVER_FEATURES  0x020  /* 驱动选择的特性 */
#define VIRTIO_MMIO_QUEUE_SEL        0x030  /* 选择哪个队列 */
#define VIRTIO_MMIO_QUEUE_NUM_MAX    0x034  /* 队列最大长度 */
#define VIRTIO_MMIO_QUEUE_NUM        0x038  /* 队列实际长度 */
#define VIRTIO_MMIO_QUEUE_READY      0x044  /* V2: 队列就绪 */
#define VIRTIO_MMIO_QUEUE_NOTIFY     0x050  /* 通知设备 */
#define VIRTIO_MMIO_INTERRUPT_STATUS 0x060  /* 中断状态 */
#define VIRTIO_MMIO_INTERRUPT_ACK    0x064  /* 确认中断 */
#define VIRTIO_MMIO_STATUS           0x070  /* 设备状态 */
```

---

## 6.4 VirtIO 设备初始化状态机

```
  ┌──────────┐    写 STATUS=0
  │  Reset   │◄────────────────┐
  └────┬─────┘                 │
       │ 写 ACKNOWLEDGE        │ 任何错误
       ▼                       │
  ┌──────────┐                 │
  │ Ack'd    │                 │
  └────┬─────┘                 │
       │ 写 DRIVER             │
       ▼                       │
  ┌──────────┐                 │
  │ Driver   │ 特性协商         │
  └────┬─────┘                 │
       │ (V2: 写 FEATURES_OK)  │
       ▼                       │
  ┌──────────┐                 │
  │ Features │ 设置队列         │
  │   OK     │                 │
  └────┬─────┘                 │
       │ 写 DRIVER_OK          │
       ▼                       │
  ┌──────────┐                 │
  │  Ready!  │ 设备可用         │
  └──────────┘─────────────────┘
```

### Version 1 (Legacy) 初始化代码

```c
int virtio_mmio_init_v1(unsigned long base, struct virtio_device *dev)
{
    /* Step 1: Reset */
    writel(0, base + VIRTIO_MMIO_STATUS);

    /* Step 2: ACKNOWLEDGE — 驱动已发现设备 */
    writel(VIRTIO_STATUS_ACKNOWLEDGE, base + VIRTIO_MMIO_STATUS);

    /* Step 3: DRIVER — 驱动已认领设备 */
    writel(VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER,
           base + VIRTIO_MMIO_STATUS);

    /* Step 4: 特性协商 */
    uint32_t dev_features = readl(base + VIRTIO_MMIO_DEVICE_FEATURES);
    uint32_t drv_features = dev_features & driver_wanted_features;
    writel(drv_features, base + VIRTIO_MMIO_DRIVER_FEATURES);

    /* Step 5: V1 特有 — 设置 Guest 页大小 */
    writel(PAGE_SIZE, base + VIRTIO_MMIO_GUEST_PAGE_SIZE);

    /* Step 6: 设置 VirtQueue（见下节） */

    /* Step 7: DRIVER_OK — 设备可用 */
    writel(VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER |
           VIRTIO_STATUS_DRIVER_OK,
           base + VIRTIO_MMIO_STATUS);

    return 0;
}
```

---

## 6.5 Vring — VirtIO 的核心数据结构

### 三部分结构

```
  ┌─────────────────────────────────────────────────┐
  │              Descriptor Table                    │
  │  [0] addr=0x... len=512 flags=NEXT  next=1      │
  │  [1] addr=0x... len=4096 flags=WRITE next=2     │
  │  [2] addr=0x... len=1   flags=WRITE next=0      │
  │  ...                                            │
  ├─────────────────────────────────────────────────┤
  │              Available Ring                      │
  │  flags=0  idx=3                                 │
  │  ring[0]=0  ring[1]=3  ring[2]=6  ...           │
  │  （驱动写，设备读）                              │
  ├─────────────────────────────────────────────────┤
  │              Used Ring                           │
  │  flags=0  idx=2                                 │
  │  ring[0]={id=0, len=512}                        │
  │  ring[1]={id=3, len=4096}                       │
  │  （设备写，驱动读）                              │
  └─────────────────────────────────────────────────┘
```

### 数据结构定义

```c
struct vring_desc {
    uint64_t addr;    /* 缓冲区物理地址 */
    uint32_t len;     /* 字节长度 */
    uint16_t flags;   /* VRING_DESC_F_NEXT, VRING_DESC_F_WRITE */
    uint16_t next;    /* 链中下一个描述符索引 */
};

struct vring_avail {
    uint16_t flags;
    uint16_t idx;             /* 下一个空闲位置 */
    uint16_t ring[QUEUE_NUM]; /* 描述符链头索引 */
};

struct vring_used {
    uint16_t flags;
    uint16_t idx;                        /* 下一个完成位置 */
    struct { uint32_t id; uint32_t len; }
        ring[QUEUE_NUM];                 /* 完成的描述符 */
};
```

### I/O 操作流程

```
  驱动 (Guest)                              设备 (QEMU)
  ═══════════                              ═══════════
  1. 填充 Descriptor Table
     (地址、长度、方向)
          │
  2. 写 Available Ring
     (avail->ring[avail->idx % N] = head)
     (avail->idx++)
          │
  3. 写 QUEUE_NOTIFY ──────────────────► 设备被通知
          │                                    │
          │                              4. 读 Descriptor Table
          │                                 执行 I/O
          │                                    │
          │                              5. 写 Used Ring
          │ ◄───────────────────────────    (used->idx++)
          │                              6. 触发 IRQ
  7. 检查 used->idx
     读取完成的缓冲区
```

---

## 6.6 virtqueue 操作实现

### 添加缓冲区

```c
int virtqueue_add_buf(struct virtqueue *vq,
                      unsigned long sg_addr[], unsigned int sg_len[],
                      unsigned int out_num, unsigned int in_num)
{
    int head = vq->free_head;
    int i = head;

    /* 链接 out 缓冲区（驱动→设备：只读） */
    for (int n = 0; n < out_num; n++) {
        vq->desc[i].addr = sg_addr[n];
        vq->desc[i].len = sg_len[n];
        vq->desc[i].flags = VRING_DESC_F_NEXT;
        i = vq->desc[i].next;
    }

    /* 链接 in 缓冲区（设备→驱动：可写） */
    for (int n = 0; n < in_num; n++) {
        vq->desc[i].addr = sg_addr[out_num + n];
        vq->desc[i].len = sg_len[out_num + n];
        vq->desc[i].flags = VRING_DESC_F_WRITE;
        if (n < in_num - 1)
            vq->desc[i].flags |= VRING_DESC_F_NEXT;
        i = vq->desc[i].next;
    }

    /* 更新 Available Ring */
    wmb();  /* 确保描述符写完 */
    vq->avail->ring[vq->avail->idx % vq->num] = head;
    wmb();
    vq->avail->idx++;

    return 0;
}
```

### 通知设备

```c
void virtqueue_kick(struct virtqueue *vq)
{
    wmb();
    writel(vq->queue_index, vq->mmio_base + VIRTIO_MMIO_QUEUE_NOTIFY);
}
```

### 获取完成的缓冲区

```c
int virtqueue_get_buf(struct virtqueue *vq, unsigned int *len)
{
    if (vq->last_used_idx == vq->used->idx)
        return -1;  /* 没有完成的 */

    rmb();
    struct vring_used_elem *elem =
        &vq->used->ring[vq->last_used_idx % vq->num];
    int head = elem->id;
    *len = elem->len;

    /* 释放描述符链 */
    free_desc_chain(vq, head);
    vq->last_used_idx++;

    return head;
}
```

---

## 6.7 VirtIO Block 设备驱动

### 块设备 I/O 请求格式

```
  描述符 0 (OUT):  virtio_blk_req header
  描述符 1 (IN/OUT): 数据缓冲区
  描述符 2 (IN):   status 字节

  ┌───────────────────┐     ┌──────────────┐     ┌────────┐
  │ type=IN/OUT       │────►│ 512B 数据    │────►│ status │
  │ sector=N          │     │              │     │ (1B)   │
  └───────────────────┘     └──────────────┘     └────────┘
  驱动→设备 (只读)          读=设备→驱动         设备→驱动
                            写=驱动→设备
```

### 读取实现

```c
int virtio_blk_read(unsigned long sector, void *buf, unsigned int len)
{
    struct virtio_blk_req req;
    unsigned char status = 0xFF;

    req.type = VIRTIO_BLK_T_IN;    /* 读 */
    req.reserved = 0;
    req.sector = sector;

    /* 3 个 scatter-gather 条目 */
    unsigned long sg_addr[3] = {
        (unsigned long)&req,     /* header */
        (unsigned long)buf,      /* 数据 */
        (unsigned long)&status   /* 状态 */
    };
    unsigned int sg_len[3] = { 16, len, 1 };

    /* out=1(header), in=2(data+status) */
    virtqueue_add_buf(&blk_vq, sg_addr, sg_len, 1, 2);
    virtqueue_kick(&blk_vq);

    /* 轮询等待完成 */
    unsigned int used_len;
    while (virtqueue_get_buf(&blk_vq, &used_len) < 0)
        ;

    return (status == VIRTIO_BLK_S_OK) ? 0 : -1;
}
```

---

## 6.8 VirtIO 设备扫描

```c
void virtio_init(void)
{
    for (int i = 0; i < 32; i++) {
        unsigned long base = VIRTIO_MMIO_BASE + (unsigned long)i * 0x200;

        /* 检查魔数 */
        if (readl(base + VIRTIO_MMIO_MAGIC_VALUE) != 0x74726976)
            continue;

        /* 检查设备 ID */
        uint32_t device_id = readl(base + VIRTIO_MMIO_DEVICE_ID);
        if (device_id == 0)
            continue;  /* 空槽 */

        /* 注册设备 */
        virtio_devices[virtio_device_count].base = base;
        virtio_devices[virtio_device_count].device_id = device_id;
        virtio_devices[virtio_device_count].version =
            readl(base + VIRTIO_MMIO_VERSION);
        virtio_device_count++;
    }
}
```

### IRQ 映射

每个 VirtIO MMIO 槽连接到 GIC SPI，中断号 = 48 + slot_index：

```c
/* QEMU virt machine: slot i → SPI(16+i) → INTID(48+i) */
unsigned int irq = 48 + slot_index;
request_irq(irq, virtio_blk_irq_handler);
gicv3_enable_spi(irq);
```

---

## 6.9 Phase 6 核心概念总结

| 概念 | 说明 |
|------|------|
| **VirtIO** | 虚拟化标准 I/O 接口 |
| **MMIO 传输层** | 通过内存映射寄存器控制设备 |
| **vring** | 描述符表 + Available Ring + Used Ring |
| **状态机初始化** | Reset → ACK → DRIVER → Features → DRIVER_OK |
| **3-描述符链** | 块设备: header → data → status |
| **kick** | 写 QUEUE_NOTIFY 通知设备有新请求 |
| **V1 vs V2** | Legacy 连续内存 vs Modern 分离地址 |

**Phase 6 奠定的基础**：有了块设备驱动，Phase 7-9 的文件系统就有了持久化存储后端。
