# Phase 6：VirtIO驱动框架（MMIO传输层 + blk + net）

## 参考内核文件

```
drivers/virtio/virtio_mmio.c        # VirtIO MMIO传输层（核心）
drivers/virtio/virtio_ring.c        # virtqueue 实现（vring操作）
drivers/virtio/virtio.c             # VirtIO总线框架
drivers/block/virtio_blk.c          # VirtIO块设备驱动
drivers/net/virtio_net.c            # VirtIO网络设备驱动
include/linux/virtio.h              # VirtIO设备/驱动接口
include/linux/virtio_mmio.h         # MMIO寄存器定义
include/uapi/linux/virtio_blk.h     # 块设备请求格式
include/uapi/linux/virtio_net.h     # 网络设备特性/头部
```

---

## 6.1 VirtIO整体架构

```
用户态进程
    │ read/write
    ▼
块设备层 (bio/request_queue)
    │
    ▼
virtio_blk 驱动         virtio_net 驱动
    │                       │
    ▼                       ▼
virtqueue (vring)       virtqueue (vring)
    │                       │
    ▼                       ▼
VirtIO MMIO 传输层（寄存器 I/O）
    │
    ▼
QEMU 后端（模拟设备）
```

## 6.2 MMIO寄存器映射（参考 virtio_mmio.h）

```c
/* QEMU virt machine: VirtIO MMIO基地址 = 0x0a000000，步进 0x200 */
/* 每个设备占 512 字节的 MMIO 空间 */

#define VIRTIO_MMIO_MAGIC_VALUE      0x000  /* 只读，必须为 0x74726976 ("virt") */
#define VIRTIO_MMIO_VERSION          0x004  /* 只读，版本号（2=现代版）*/
#define VIRTIO_MMIO_DEVICE_ID        0x008  /* 只读，设备类型（1=net, 2=blk）*/
#define VIRTIO_MMIO_VENDOR_ID        0x00c
#define VIRTIO_MMIO_DEVICE_FEATURES  0x010  /* 只读，设备支持的特性位 */
#define VIRTIO_MMIO_FEATURES_SEL     0x014  /* 写：选择高32位或低32位特性 */
#define VIRTIO_MMIO_DRIVER_FEATURES  0x020  /* 写：驱动接受的特性位 */
#define VIRTIO_MMIO_DRIVER_FEATURES_SEL 0x024
#define VIRTIO_MMIO_QUEUE_SEL        0x030  /* 写：选择操作的队列号 */
#define VIRTIO_MMIO_QUEUE_NUM_MAX    0x034  /* 只读：队列最大容量 */
#define VIRTIO_MMIO_QUEUE_NUM        0x038  /* 写：实际使用的队列大小 */
#define VIRTIO_MMIO_QUEUE_READY      0x044  /* 写1表示队列就绪 */
#define VIRTIO_MMIO_QUEUE_NOTIFY     0x050  /* 写：通知设备有新请求 */
#define VIRTIO_MMIO_INTERRUPT_STATUS 0x060  /* 只读：中断原因 */
#define VIRTIO_MMIO_INTERRUPT_ACK    0x064  /* 写：清除中断 */
#define VIRTIO_MMIO_STATUS           0x070  /* 设备状态机 */
#define VIRTIO_MMIO_QUEUE_DESC_LOW   0x080  /* 描述符表物理地址（低32位）*/
#define VIRTIO_MMIO_QUEUE_DESC_HIGH  0x084
#define VIRTIO_MMIO_QUEUE_AVAIL_LOW  0x090  /* Available Ring物理地址 */
#define VIRTIO_MMIO_QUEUE_AVAIL_HIGH 0x094
#define VIRTIO_MMIO_QUEUE_USED_LOW   0x0a0  /* Used Ring物理地址 */
#define VIRTIO_MMIO_QUEUE_USED_HIGH  0x0a4

/* 设备状态位（按顺序设置，参考 virtio spec）*/
#define VIRTIO_STATUS_ACKNOWLEDGE    1   /* 驱动发现了设备 */
#define VIRTIO_STATUS_DRIVER         2   /* 驱动知道如何驱动该设备 */
#define VIRTIO_STATUS_FEATURES_OK    8   /* 特性协商完成 */
#define VIRTIO_STATUS_DRIVER_OK      4   /* 驱动完全就绪 */
#define VIRTIO_STATUS_FAILED         128 /* 出现错误 */
```

## 6.3 virtqueue（vring）数据结构

```c
/* 参考 include/uapi/linux/virtio_ring.h */
/* vring 由三部分组成，均需页对齐 */

/* 1. 描述符表（Descriptor Table）*/
struct vring_desc {
    uint64_t addr;   /* 缓冲区物理地址 */
    uint32_t len;    /* 缓冲区长度 */
    uint16_t flags;  /* VRING_DESC_F_NEXT=1(链式), VRING_DESC_F_WRITE=2(设备写) */
    uint16_t next;   /* 下一个描述符索引（flags & NEXT 时有效）*/
};

/* 2. Available Ring（驱动→设备）*/
struct vring_avail {
    uint16_t flags;  /* VRING_AVAIL_F_NO_INTERRUPT=1 */
    uint16_t idx;    /* 下一个可用槽的索引（单调递增）*/
    uint16_t ring[]; /* 描述符链头的索引数组 */
};

/* 3. Used Ring（设备→驱动）*/
struct vring_used_elem {
    uint32_t id;  /* 已完成的描述符链头索引 */
    uint32_t len; /* 设备实际写入的字节数 */
};

struct vring_used {
    uint16_t flags;
    uint16_t idx;            /* 单调递增 */
    struct vring_used_elem ring[];
};

/* virtqueue 管理结构 */
struct virtqueue {
    void        *mmio_base;      /* MMIO基地址 */
    uint16_t     queue_index;    /* 队列号 */
    uint16_t     num;            /* 队列容量（2的幂次）*/
    /* 三张表的虚拟地址 */
    struct vring_desc  *desc;
    struct vring_avail *avail;
    struct vring_used  *used;
    /* 空闲描述符链表 */
    uint16_t     free_head;
    uint16_t     num_free;
    /* 上次处理的 used->idx */
    uint16_t     last_used_idx;
};
```

## 6.4 VirtIO设备初始化流程

```c
/* 参考 drivers/virtio/virtio_mmio.c */

int virtio_mmio_init(void *mmio_base, int device_id) {
    uint32_t magic = readl(mmio_base + VIRTIO_MMIO_MAGIC_VALUE);
    if (magic != 0x74726976) return -ENODEV;

    /* Step 1: ACKNOWLEDGE — 告知设备驱动已发现它 */
    writel(VIRTIO_STATUS_ACKNOWLEDGE, mmio_base + VIRTIO_MMIO_STATUS);

    /* Step 2: DRIVER — 告知设备驱动已准备好 */
    uint32_t status = readl(mmio_base + VIRTIO_MMIO_STATUS);
    writel(status | VIRTIO_STATUS_DRIVER, mmio_base + VIRTIO_MMIO_STATUS);

    /* Step 3: 特性协商 */
    /* 读取设备特性（先选低32位）*/
    writel(0, mmio_base + VIRTIO_MMIO_FEATURES_SEL);
    uint32_t features = readl(mmio_base + VIRTIO_MMIO_DEVICE_FEATURES);

    /* 驱动接受的特性（根据设备类型选择）*/
    writel(0, mmio_base + VIRTIO_MMIO_DRIVER_FEATURES_SEL);
    writel(features & DRIVER_SUPPORTED_FEATURES,
           mmio_base + VIRTIO_MMIO_DRIVER_FEATURES);

    /* Step 4: FEATURES_OK */
    status = readl(mmio_base + VIRTIO_MMIO_STATUS);
    writel(status | VIRTIO_STATUS_FEATURES_OK, mmio_base + VIRTIO_MMIO_STATUS);
    /* 重读确认设备接受了特性 */
    if (!(readl(mmio_base + VIRTIO_MMIO_STATUS) & VIRTIO_STATUS_FEATURES_OK))
        return -EINVAL;

    /* Step 5: 初始化队列 */
    setup_virtqueue(mmio_base, 0 /* queue_index */);

    /* Step 6: DRIVER_OK */
    status = readl(mmio_base + VIRTIO_MMIO_STATUS);
    writel(status | VIRTIO_STATUS_DRIVER_OK, mmio_base + VIRTIO_MMIO_STATUS);
    return 0;
}

void setup_virtqueue(void *mmio_base, int queue_index) {
    /* 选择队列 */
    writel(queue_index, mmio_base + VIRTIO_MMIO_QUEUE_SEL);

    uint32_t max = readl(mmio_base + VIRTIO_MMIO_QUEUE_NUM_MAX);
    uint16_t num = min(max, 256);  /* 选用256项 */
    writel(num, mmio_base + VIRTIO_MMIO_QUEUE_NUM);

    /* 分配三张表（需页对齐）*/
    struct vring_desc  *desc  = alloc_pages_aligned(PAGE_SIZE);
    struct vring_avail *avail = (void *)((char *)desc + num * 16);
    struct vring_used  *used  = alloc_pages_aligned(PAGE_SIZE);

    /* 告诉设备三张表的物理地址 */
    uint64_t desc_pa = virt_to_phys(desc);
    writel((uint32_t)desc_pa,        mmio_base + VIRTIO_MMIO_QUEUE_DESC_LOW);
    writel((uint32_t)(desc_pa >> 32), mmio_base + VIRTIO_MMIO_QUEUE_DESC_HIGH);

    uint64_t avail_pa = virt_to_phys(avail);
    writel((uint32_t)avail_pa,        mmio_base + VIRTIO_MMIO_QUEUE_AVAIL_LOW);
    writel((uint32_t)(avail_pa >> 32), mmio_base + VIRTIO_MMIO_QUEUE_AVAIL_HIGH);

    uint64_t used_pa = virt_to_phys(used);
    writel((uint32_t)used_pa,        mmio_base + VIRTIO_MMIO_QUEUE_USED_LOW);
    writel((uint32_t)(used_pa >> 32), mmio_base + VIRTIO_MMIO_QUEUE_USED_HIGH);

    /* 激活队列 */
    writel(1, mmio_base + VIRTIO_MMIO_QUEUE_READY);
}
```

## 6.5 VirtIO块设备读写（参考 virtio_blk.c）

```c
/* 参考 include/uapi/linux/virtio_blk.h */
#define VIRTIO_BLK_T_IN   0  /* 读（设备→驱动）*/
#define VIRTIO_BLK_T_OUT  1  /* 写（驱动→设备）*/

struct virtio_blk_req {
    uint32_t type;    /* VIRTIO_BLK_T_IN 或 OUT */
    uint32_t reserved;
    uint64_t sector;  /* 起始扇区号（512字节/扇区）*/
};

/* 一次块请求需要3个描述符构成链：
 * desc[0] -> virtio_blk_req（驱动→设备，只读）
 * desc[1] -> 数据缓冲区（读：设备→驱动可写；写：驱动→设备只读）
 * desc[2] -> status字节（设备→驱动，1字节，0=成功）
 */
int virtio_blk_read(struct virtqueue *vq, uint64_t sector,
                    void *buf, uint32_t len) {
    struct virtio_blk_req req = {
        .type = VIRTIO_BLK_T_IN,
        .sector = sector,
    };
    uint8_t status = 0xFF;

    /* 分配3个描述符 */
    uint16_t head = alloc_desc_chain(vq, 3);
    uint16_t d0 = head, d1 = vq->desc[d0].next, d2 = vq->desc[d1].next;

    /* 描述符0：请求头（只读）*/
    vq->desc[d0].addr  = virt_to_phys(&req);
    vq->desc[d0].len   = sizeof(req);
    vq->desc[d0].flags = VRING_DESC_F_NEXT;

    /* 描述符1：数据缓冲区（设备写入）*/
    vq->desc[d1].addr  = virt_to_phys(buf);
    vq->desc[d1].len   = len;
    vq->desc[d1].flags = VRING_DESC_F_NEXT | VRING_DESC_F_WRITE;

    /* 描述符2：状态字节（设备写入）*/
    vq->desc[d2].addr  = virt_to_phys(&status);
    vq->desc[d2].len   = 1;
    vq->desc[d2].flags = VRING_DESC_F_WRITE;

    /* 提交到 Available Ring */
    uint16_t avail_idx = vq->avail->idx % vq->num;
    vq->avail->ring[avail_idx] = head;
    wmb();  /* 确保描述符写入对设备可见 */
    vq->avail->idx++;
    wmb();

    /* 通知设备（写 QUEUE_NOTIFY）*/
    writel(vq->queue_index, vq->mmio_base + VIRTIO_MMIO_QUEUE_NOTIFY);

    /* 轮询 Used Ring 等待完成 */
    while (vq->used->idx == vq->last_used_idx)
        cpu_relax();
    vq->last_used_idx++;

    return (status == 0) ? 0 : -EIO;
}
```

## 6.6 验证方法

```bash
# QEMU 启动参数（添加virtio块设备）
qemu-system-aarch64 \
    -M virt \
    -cpu cortex-a72 \
    -m 1G \
    -kernel arm64os.elf \
    -drive file=disk.img,format=raw,if=none,id=blk0 \
    -device virtio-blk-device,drive=blk0 \
    -device virtio-net-device,netdev=net0 \
    -netdev user,id=net0 \
    -nographic
```

```c
/* 验证代码 */
void test_virtio_blk(void) {
    uint8_t buf[512];
    /* 读取第0扇区（MBR）*/
    int ret = virtio_blk_read(&blk_vq, 0, buf, 512);
    if (ret == 0)
        printk("VirtIO blk read OK: %02x %02x %02x...\n",
               buf[0], buf[1], buf[2]);
}
```

## 6.7 本阶段产出文件

```
arm64os/
└── drivers/
    ├── virtio/
    │   ├── virtio_mmio.c    ← MMIO传输层（核心）
    │   ├── virtio_ring.c    ← virtqueue/vring操作
    │   └── virtio.c         ← VirtIO总线框架
    ├── block/
    │   └── virtio_blk.c     ← 块设备驱动
    └── net/
        └── virtio_net.c     ← 网络设备驱动
```
