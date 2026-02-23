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
/* QEMU 8.x virt machine 使用 Version 1 (Legacy)，以下寄存器覆盖两个版本 */

/* 公共寄存器（Version 1 和 Version 2 共用） */
#define VIRTIO_MMIO_MAGIC_VALUE         0x000  /* 只读，必须为 0x74726976 ("virt") */
#define VIRTIO_MMIO_VERSION             0x004  /* 只读，版本号（1=legacy, 2=modern） */
#define VIRTIO_MMIO_DEVICE_ID           0x008  /* 只读，设备类型（1=net, 2=blk） */
#define VIRTIO_MMIO_VENDOR_ID           0x00c  /* 只读，厂商 ID */
#define VIRTIO_MMIO_DEVICE_FEATURES     0x010  /* 只读，设备支持的特性位 */
#define VIRTIO_MMIO_DEVICE_FEATURES_SEL 0x014  /* 写：选择高32位或低32位特性 */
#define VIRTIO_MMIO_DRIVER_FEATURES     0x020  /* 写：驱动接受的特性位 */
#define VIRTIO_MMIO_DRIVER_FEATURES_SEL 0x024  /* 写：选择高32位或低32位特性 */
#define VIRTIO_MMIO_GUEST_PAGE_SIZE     0x028  /* V1: 写：客户机页大小（V1必须设置） */
#define VIRTIO_MMIO_QUEUE_SEL           0x030  /* 写：选择操作的队列号 */
#define VIRTIO_MMIO_QUEUE_NUM_MAX       0x034  /* 只读：队列最大容量 */
#define VIRTIO_MMIO_QUEUE_NUM           0x038  /* 写：实际使用的队列大小 */
#define VIRTIO_MMIO_QUEUE_ALIGN         0x03c  /* V1: 写：vring 对齐要求（通常 PAGE_SIZE） */
#define VIRTIO_MMIO_QUEUE_PFN           0x040  /* V1: 写：vring 物理页帧号（phys/page_size） */
#define VIRTIO_MMIO_QUEUE_READY         0x044  /* V2: 写1表示队列就绪 */
#define VIRTIO_MMIO_QUEUE_NOTIFY        0x050  /* 写：通知设备有新请求 */
#define VIRTIO_MMIO_INTERRUPT_STATUS    0x060  /* 只读：中断原因 */
#define VIRTIO_MMIO_INTERRUPT_ACK       0x064  /* 写：清除中断 */
#define VIRTIO_MMIO_STATUS              0x070  /* 设备状态机 */

/* Version 2 (Modern) 独有寄存器 */
#define VIRTIO_MMIO_QUEUE_DESC_LOW      0x080  /* V2: 描述符表物理地址（低32位） */
#define VIRTIO_MMIO_QUEUE_DESC_HIGH     0x084
#define VIRTIO_MMIO_QUEUE_AVAIL_LOW     0x090  /* V2: Available Ring物理地址 */
#define VIRTIO_MMIO_QUEUE_AVAIL_HIGH    0x094
#define VIRTIO_MMIO_QUEUE_USED_LOW      0x0a0  /* V2: Used Ring物理地址 */
#define VIRTIO_MMIO_QUEUE_USED_HIGH     0x0a4
#define VIRTIO_MMIO_CONFIG              0x100  /* 设备特定配置空间 */

/* 设备状态位（按顺序设置，参考 virtio spec） */
#define VIRTIO_STATUS_ACKNOWLEDGE    1   /* 驱动发现了设备 */
#define VIRTIO_STATUS_DRIVER         2   /* 驱动知道如何驱动该设备 */
#define VIRTIO_STATUS_DRIVER_OK      4   /* 驱动完全就绪 */
#define VIRTIO_STATUS_FEATURES_OK    8   /* 特性协商完成（仅 V2） */
#define VIRTIO_STATUS_FAILED         128 /* 出现错误 */
```

### 6.2.1 Version 1 (Legacy) 与 Version 2 (Modern) 关键差异

| 特性 | Version 1 (Legacy) | Version 2 (Modern) |
|------|-------------------|-------------------|
| 队列设置 | GUEST_PAGE_SIZE + QUEUE_ALIGN + QUEUE_PFN | QUEUE_DESC/AVAIL/USED + QUEUE_READY |
| vring 布局 | 连续内存（desc + avail + padding + used） | desc/avail/used 可独立分配 |
| 特性协商 | 无 FEATURES_OK 步骤 | 有 FEATURES_OK 确认步骤 |
| QEMU virt | **默认使用**（QEMU 8.x） | 需显式配置 |

## 6.3 virtqueue（vring）数据结构

```c
/* 参考 include/uapi/linux/virtio_ring.h */
/* vring 由三部分组成 */

/* 1. 描述符表（Descriptor Table） */
struct vring_desc {
    uint64_t addr;   /* 缓冲区物理地址 */
    uint32_t len;    /* 缓冲区长度 */
    uint16_t flags;  /* VRING_DESC_F_NEXT=1(链式), VRING_DESC_F_WRITE=2(设备写) */
    uint16_t next;   /* 下一个描述符索引（flags & NEXT 时有效） */
};

/* 2. Available Ring（驱动→设备） */
struct vring_avail {
    uint16_t flags;  /* VRING_AVAIL_F_NO_INTERRUPT=1 */
    uint16_t idx;    /* 下一个可用槽的索引（单调递增） */
    uint16_t ring[]; /* 描述符链头的索引数组 */
};

/* 3. Used Ring（设备→驱动） */
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
    uint16_t     num;            /* 队列容量（2的幂次） */
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

### 6.3.1 Version 1 (Legacy) vring 内存布局

```
Version 1 要求 desc + avail + used 在一块连续的页对齐物理内存中：

┌─────────────────────────────────────┐  ← vring 基址（页对齐）
│  Descriptor Table                   │     num × 16 bytes
│  (struct vring_desc × num)          │
├─────────────────────────────────────┤
│  Available Ring                     │     4 + 2×num + 2 bytes
│  (flags, idx, ring[num], used_event)│
├── ALIGN to PAGE_SIZE ───────────────┤  ← 对齐到 QUEUE_ALIGN 边界
│  Used Ring                          │     4 + 8×num + 2 bytes
│  (flags, idx, ring[num], avail_event)│
└─────────────────────────────────────┘

设备通过 QUEUE_PFN = physical_address / PAGE_SIZE 获取基址。
```

## 6.4 VirtIO设备初始化流程

### 6.4.1 Version 1 (Legacy) 初始化

```c
/* 参考 drivers/virtio/virtio_mmio.c (legacy path) */
/* QEMU 8.x virt machine 默认使用此路径 */

int virtio_mmio_init_v1(void *mmio_base) {
    uint32_t magic = readl(mmio_base + VIRTIO_MMIO_MAGIC_VALUE);
    if (magic != 0x74726976) return -1;

    /* Step 1: Reset — 写 0 重置设备 */
    writel(0, mmio_base + VIRTIO_MMIO_STATUS);

    /* Step 2: ACKNOWLEDGE — 告知设备驱动已发现它 */
    writel(VIRTIO_STATUS_ACKNOWLEDGE, mmio_base + VIRTIO_MMIO_STATUS);

    /* Step 3: DRIVER — 告知设备驱动已准备好 */
    uint32_t status = readl(mmio_base + VIRTIO_MMIO_STATUS);
    writel(status | VIRTIO_STATUS_DRIVER, mmio_base + VIRTIO_MMIO_STATUS);

    /* Step 4: 特性协商（V1 无 FEATURES_OK 步骤） */
    writel(0, mmio_base + VIRTIO_MMIO_DEVICE_FEATURES_SEL);
    uint32_t features = readl(mmio_base + VIRTIO_MMIO_DEVICE_FEATURES);
    writel(0, mmio_base + VIRTIO_MMIO_DRIVER_FEATURES_SEL);
    writel(features & DRIVER_SUPPORTED_FEATURES,
           mmio_base + VIRTIO_MMIO_DRIVER_FEATURES);

    /* Step 5: 设置 GUEST_PAGE_SIZE（V1 必须） */
    writel(PAGE_SIZE, mmio_base + VIRTIO_MMIO_GUEST_PAGE_SIZE);

    /* Step 6: 初始化队列（V1 连续 vring 布局） */
    setup_virtqueue_v1(mmio_base, 0);

    /* Step 7: DRIVER_OK */
    status = readl(mmio_base + VIRTIO_MMIO_STATUS);
    writel(status | VIRTIO_STATUS_DRIVER_OK, mmio_base + VIRTIO_MMIO_STATUS);
    return 0;
}

void setup_virtqueue_v1(void *mmio_base, int queue_index) {
    writel(queue_index, mmio_base + VIRTIO_MMIO_QUEUE_SEL);

    uint32_t max = readl(mmio_base + VIRTIO_MMIO_QUEUE_NUM_MAX);
    uint16_t num = min(max, 256);
    writel(num, mmio_base + VIRTIO_MMIO_QUEUE_NUM);

    /* 分配连续 vring 内存（desc + avail + padding + used） */
    size_t total = vring_size(num, PAGE_SIZE);
    void *vring = alloc_pages_aligned(total);
    memset(vring, 0, total);

    /* desc 在 vring 起始处 */
    struct vring_desc *desc = (struct vring_desc *)vring;
    /* avail 紧跟 desc 之后 */
    struct vring_avail *avail = (void *)((char *)vring + num * 16);
    /* used 在 ALIGN(desc+avail, PAGE_SIZE) 处 */
    size_t used_offset = ALIGN(num * 16 + 6 + 2 * num, PAGE_SIZE);
    struct vring_used *used = (void *)((char *)vring + used_offset);

    /* V1: 设置对齐和 PFN */
    writel(PAGE_SIZE, mmio_base + VIRTIO_MMIO_QUEUE_ALIGN);
    writel(virt_to_phys(vring) / PAGE_SIZE,
           mmio_base + VIRTIO_MMIO_QUEUE_PFN);
}
```

### 6.4.2 Version 2 (Modern) 初始化

```c
/* 参考 drivers/virtio/virtio_mmio.c (modern path) */

int virtio_mmio_init_v2(void *mmio_base) {
    /* Steps 1-3: 同 V1（Reset → ACKNOWLEDGE → DRIVER） */

    /* Step 4: 特性协商 */
    writel(0, mmio_base + VIRTIO_MMIO_DEVICE_FEATURES_SEL);
    uint32_t features = readl(mmio_base + VIRTIO_MMIO_DEVICE_FEATURES);
    writel(0, mmio_base + VIRTIO_MMIO_DRIVER_FEATURES_SEL);
    writel(features & DRIVER_SUPPORTED_FEATURES,
           mmio_base + VIRTIO_MMIO_DRIVER_FEATURES);

    /* Step 5: FEATURES_OK（V2 独有） */
    uint32_t status = readl(mmio_base + VIRTIO_MMIO_STATUS);
    writel(status | VIRTIO_STATUS_FEATURES_OK, mmio_base + VIRTIO_MMIO_STATUS);
    if (!(readl(mmio_base + VIRTIO_MMIO_STATUS) & VIRTIO_STATUS_FEATURES_OK))
        return -1;  /* 设备不接受特性 */

    /* Step 6: 初始化队列（V2 独立地址） */
    setup_virtqueue_v2(mmio_base, 0);

    /* Step 7: DRIVER_OK */
    status = readl(mmio_base + VIRTIO_MMIO_STATUS);
    writel(status | VIRTIO_STATUS_DRIVER_OK, mmio_base + VIRTIO_MMIO_STATUS);
    return 0;
}

void setup_virtqueue_v2(void *mmio_base, int queue_index) {
    writel(queue_index, mmio_base + VIRTIO_MMIO_QUEUE_SEL);

    uint32_t max = readl(mmio_base + VIRTIO_MMIO_QUEUE_NUM_MAX);
    uint16_t num = min(max, 256);
    writel(num, mmio_base + VIRTIO_MMIO_QUEUE_NUM);

    /* V2: desc/avail/used 可独立分配 */
    struct vring_desc  *desc  = alloc_pages_aligned(PAGE_SIZE);
    struct vring_avail *avail = (void *)((char *)desc + num * 16);
    struct vring_used  *used  = alloc_pages_aligned(PAGE_SIZE);

    uint64_t desc_pa = virt_to_phys(desc);
    writel((uint32_t)desc_pa,        mmio_base + VIRTIO_MMIO_QUEUE_DESC_LOW);
    writel((uint32_t)(desc_pa >> 32), mmio_base + VIRTIO_MMIO_QUEUE_DESC_HIGH);

    uint64_t avail_pa = virt_to_phys(avail);
    writel((uint32_t)avail_pa,        mmio_base + VIRTIO_MMIO_QUEUE_AVAIL_LOW);
    writel((uint32_t)(avail_pa >> 32), mmio_base + VIRTIO_MMIO_QUEUE_AVAIL_HIGH);

    uint64_t used_pa = virt_to_phys(used);
    writel((uint32_t)used_pa,        mmio_base + VIRTIO_MMIO_QUEUE_USED_LOW);
    writel((uint32_t)(used_pa >> 32), mmio_base + VIRTIO_MMIO_QUEUE_USED_HIGH);

    writel(1, mmio_base + VIRTIO_MMIO_QUEUE_READY);
}
```

## 6.5 VirtIO块设备读写（参考 virtio_blk.c）

```c
/* 参考 include/uapi/linux/virtio_blk.h */
#define VIRTIO_BLK_T_IN   0  /* 读（设备→驱动） */
#define VIRTIO_BLK_T_OUT  1  /* 写（驱动→设备） */

struct virtio_blk_req {
    uint32_t type;    /* VIRTIO_BLK_T_IN 或 OUT */
    uint32_t reserved;
    uint64_t sector;  /* 起始扇区号（512字节/扇区） */
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

    /* 使用 virtqueue_add_buf 提交 scatter-gather 列表 */
    uint64_t sg_addr[3] = {
        virt_to_phys(&req),
        virt_to_phys(buf),
        virt_to_phys(&status)
    };
    uint32_t sg_len[3] = { sizeof(req), len, 1 };

    /* out_num=1（req 只读），in_num=2（data + status 设备可写） */
    virtqueue_add_buf(vq, sg_addr, sg_len, 1, 2);

    /* 通知设备（写 QUEUE_NOTIFY） */
    virtqueue_kick(vq);

    /* 轮询 Used Ring 等待完成 */
    while (virtqueue_get_buf(vq, NULL) < 0)
        cpu_relax();

    return (status == 0) ? 0 : -EIO;
}
```

## 6.6 验证方法

```bash
# QEMU 启动参数（添加virtio块设备和网络设备）
# 注：-M virt,gic-version=3 指定 GIC v3（与 Phase 3 一致）
qemu-system-aarch64 \
    -M virt,gic-version=3 \
    -cpu cortex-a72 \
    -m 1G \
    -kernel arm64os.elf \
    -drive file=disk.img,format=raw,if=none,id=blk0 \
    -device virtio-blk-device,drive=blk0 \
    -device virtio-net-device,netdev=net0 \
    -netdev user,id=net0 \
    -nographic
```

```bash
# 创建测试磁盘映像（1MB，第0扇区写入 "TALOS" 签名）
dd if=/dev/zero of=disk.img bs=1M count=1
printf 'TALOS' | dd of=disk.img bs=1 conv=notrunc
```

```c
/* 验证代码 */
void test_virtio_blk(void) {
    uint8_t buf[512];

    /* 读取第0扇区 */
    int ret = virtio_blk_read(0, buf, 512);
    if (ret == 0)
        /* 应输出 "54 41 4c 4f 53"（"TALOS" 的 ASCII） */
        printk("VirtIO blk read OK: %02x %02x %02x...\n",
               buf[0], buf[1], buf[2]);

    /* 写入扇区1，回读验证 */
    memset(write_buf, pattern, 512);
    virtio_blk_write(1, write_buf, 512);
    virtio_blk_read(1, read_buf, 512);
    assert(memcmp(write_buf, read_buf, 512) == 0);
}

void test_virtio_net(void) {
    /* 验证 MAC 地址可读（QEMU 默认 52:54:00:12:34:56） */
    /* 验证设备状态为 DRIVER_OK */
    /* 完整收发测试在 Phase 11（TCP/IP）中实现 */
}
```

### 6.6.1 QEMU VirtIO MMIO IRQ 映射

```
QEMU virt machine VirtIO MMIO 设备映射：
  slot i → MMIO 基址 0x0a000000 + i * 0x200
         → SPI (16 + i) → GIC INTID (48 + i)

  slot 0-29:  未使用
  slot 30:    第二个 -device（如 virtio-net-device）
  slot 31:    第一个 -device（如 virtio-blk-device）
```

## 6.7 本阶段产出文件

```
arm64os/
├── include/
│   └── linux/
│       ├── io.h             ← MMIO读写辅助函数 + 内存屏障（新增，共享）
│       ├── virtio.h         ← VirtIO设备/驱动接口
│       ├── virtio_mmio.h    ← MMIO寄存器定义（V1 + V2）
│       ├── virtio_ring.h    ← vring数据结构与virtqueue接口
│       ├── virtio_blk.h     ← 块设备请求格式
│       └── virtio_net.h     ← 网络设备特性/头部
└── drivers/
    ├── virtio/
    │   ├── virtio_mmio.c    ← MMIO传输层（V1/V2双路径，核心）
    │   ├── virtio_ring.c    ← virtqueue/vring操作（V1连续布局 + V2独立地址）
    │   └── virtio.c         ← VirtIO总线框架（设备探测与注册）
    ├── block/
    │   └── virtio_blk.c     ← 块设备驱动（同步读写，3描述符链）
    └── net/
        └── virtio_net.c     ← 网络设备驱动（初始化 + MAC读取，Phase 11完善）
```
