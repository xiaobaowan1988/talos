# Phase 8：squashfs只读层 + XFS日志文件系统

> **设计说明**：本阶段选择 squashfs + XFS，而非 ext4/jbd2。
> - squashfs：容器镜像的只读压缩层，与 Phase 9 的 overlayfs 形成完整容器存储链路
> - XFS：含完整 WAL 日志，功能完全覆盖 ext4，且是 Talos Linux 的默认文件系统
> - ext4/jbd2 约 58,000 行代码，学习收益低于 XFS（约 85,000 行但设计更现代）

## 参考内核文件

```
fs/squashfs/super.c         # squashfs超级块读取
fs/squashfs/inode.c         # squashfs inode解析
fs/squashfs/dir.c           # 目录读取
fs/squashfs/file.c          # 文件数据读取（含解压）
fs/squashfs/decompressor.c  # 解压器接口（zlib/lz4/zstd）
fs/squashfs/block.c         # 数据块读取与缓存
include/linux/squashfs_fs.h # squashfs磁盘格式定义
fs/xfs/xfs_super.c          # XFS超级块（AG结构）
fs/xfs/xfs_log.c            # WAL日志核心
fs/xfs/xfs_inode.c          # XFS inode操作
fs/xfs/xfs_alloc.c          # 空间分配（AG free space B+树）
fs/xfs/xfs_dir2.c           # 目录B+树
```

---

## 8.0 VFS 增强：多挂载点支持

Phase 7 的 VFS 仅支持根挂载点（"/"）。Phase 8 需要将 squashfs 挂载到 `/sq`，
XFS 挂载到 `/xfs`，因此需要增强 VFS 支持多挂载点。

```
挂载点表（mount table）：

┌─────────────────────────────────────────┐
│ mount_table[0]: "/" → ramfs root        │
│ mount_table[1]: "/sq" → squashfs root   │
│ mount_table[2]: "/xfs" → XFS root       │
└─────────────────────────────────────────┘
```

### 关键实现

**fs/vfs/super.c 增强**：
- `do_mount()` 支持挂载到 "/" 以外的路径
- 在根 ramfs 中创建挂载点目录（如 `/sq`、`/xfs`）
- 维护全局 `mount_table[]` 数组（最多 MAX_MOUNTS=8 个挂载点）

**fs/vfs/namei.c 增强**：
- `path_lookup()` 在解析路径时检查挂载点表
- 匹配最长前缀挂载点，切换到对应文件系统的 dentry 树

```c
/* 挂载点表项 */
struct mount_entry {
    const char *mnt_path;       /* 挂载路径（如 "/sq"） */
    int         mnt_pathlen;    /* 路径长度 */
    struct vfsmount mnt;        /* 挂载信息 */
    int         used;           /* 是否已使用 */
};

#define MAX_MOUNTS 8
static struct mount_entry mount_table[MAX_MOUNTS];
```

---

## 8.1 磁盘分区方案

使用单个 virtio 块设备（disk.img），按扇区偏移划分区域：

```
disk.img 布局（8MB = 16384 sectors × 512 bytes）：

┌──────────────────────────────────┐ sector 0
│ 通用测试区（Phase 6 遗留）       │ 0 - 2047 (1MB)
│ sector 0: "TALOS" 签名          │
├──────────────────────────────────┤ sector 2048
│ squashfs 分区                    │ 2048 - 6143 (2MB)
│ 由内核 squashfs_mkfs_test()     │
│ 写入测试镜像                     │
├──────────────────────────────────┤ sector 6144
│ XFS 分区                        │ 6144 - 16383 (5MB)
│ 由内核 xfs_mkfs() 格式化        │
└──────────────────────────────────┘

#define SQFS_PART_START   2048    /* squashfs 起始扇区 */
#define SQFS_PART_SECTORS 4096    /* squashfs 扇区数（2MB）*/
#define XFS_PART_START    6144    /* XFS 起始扇区 */
#define XFS_PART_SECTORS  10240   /* XFS 扇区数（5MB）*/
```

所有文件系统通过 `virtio_blk_read(sector, buf, len)` /
`virtio_blk_write(sector, buf, len)` 访问磁盘，扇区号需加上分区起始偏移。

---

## 8.2 squashfs磁盘格式

squashfs 是一种压缩只读文件系统，广泛用于容器镜像的只读层。

```
squashfs磁盘布局（简化教学版）：
┌─────────────────────────────────────┐ 偏移 0（相对分区起始）
│  superblock（96字节）               │
├─────────────────────────────────────┤ block_size 对齐
│  数据块（Data Blocks）              │ 文件内容，按块存储
│  每块默认 4KB，可压缩或未压缩       │
├─────────────────────────────────────┤
│  Inode Table（未压缩）             │ inode 元数据数组
├─────────────────────────────────────┤
│  Directory Table（未压缩）         │ 目录项数组
└─────────────────────────────────────┘
```

```c
/* include/linux/squashfs_fs.h */

#define SQUASHFS_MAGIC          0x73717368  /* "sqsh" */
#define SQUASHFS_COMPRESSED_BIT 0x8000      /* 块大小高位=未压缩标志 */

/* squashfs 超级块（96字节，小端序 — ARM64 原生字节序） */
struct squashfs_super_block {
    u32 s_magic;             /* 0x73717368 ("sqsh") */
    u32 inodes;              /* inode 总数 */
    u32 mkfs_time;           /* 创建时间戳 */
    u32 block_size;          /* 数据块大小（默认 4096）*/
    u32 fragments;           /* 碎片块数量 */
    u16 compression;         /* 压缩算法：0=none,1=zlib,5=lz4,6=zstd */
    u16 block_log;           /* block_size = 2^block_log */
    u16 flags;               /* 标志位 */
    u16 no_ids;              /* UID/GID 表项数 */
    u16 s_major;             /* 版本主号（4）*/
    u16 s_minor;             /* 版本次号（0）*/
    u64 root_inode;          /* 根目录 inode 在 inode table 中的偏移 */
    u64 bytes_used;          /* 文件系统已使用总字节数 */
    u64 id_table_start;      /* （未使用）*/
    u64 xattr_id_table_start;/* （未使用）*/
    u64 inode_table_start;   /* inode table 的磁盘字节偏移 */
    u64 directory_table_start;/* directory table 的磁盘字节偏移 */
    u64 fragment_table_start; /* （未使用）*/
    u64 lookup_table_start;   /* （未使用）*/
};

/* squashfs inode 类型 */
#define SQUASHFS_DIR_TYPE   1   /* 目录 */
#define SQUASHFS_REG_TYPE   2   /* 普通文件 */

/* squashfs inode（简化固定大小，48字节） */
struct squashfs_inode {
    u16 inode_type;          /* SQUASHFS_DIR_TYPE 或 SQUASHFS_REG_TYPE */
    u16 mode;                /* 权限位（如 0755）*/
    u32 inode_number;        /* inode 编号 */
    u32 file_size;           /* 文件大小（字节）*/
    u32 parent_inode;        /* 父目录 inode 号 */
    u64 start_block;         /* 文件数据起始块的字节偏移（相对分区）*/
    u32 block_count;         /* 数据块数量 */
    u32 dir_offset;          /* 目录项在 directory table 中的字节偏移 */
    u32 dir_size;            /* 目录表项总大小（字节）*/
    u32 _pad;                /* 对齐填充 */
};

/* squashfs 目录项（变长，name 最长 256 字节） */
struct squashfs_dir_entry {
    u32 inode_number;        /* 对应 inode 编号 */
    u16 inode_type;          /* 类型 */
    u16 name_size;           /* 文件名长度（不含 '\0'）*/
    char name[];             /* 文件名（不含 '\0'）*/
};
```

## 8.3 squashfs数据读取流程

```c
/* 参考 fs/squashfs/file.c */
/*
 * squashfs 文件读取关键路径：
 * do_sys_open() → path_lookup() → squashfs_lookup()
 * vfs_read() → squashfs_read()
 *   → 计算数据块偏移
 *   → virtio_blk_read() 读取原始块
 *   → 如果压缩则调用 squashfs_decompress()
 *   → 拷贝到用户缓冲区
 */

static ssize_t squashfs_read(struct file *filp, char *buf,
                              size_t count, unsigned long *pos)
{
    struct inode *inode = filp->f_inode;
    struct squashfs_inode_info *si = inode->i_private;

    /* 计算该偏移属于哪个数据块 */
    u32 block_idx = *pos / si->block_size;
    u32 block_offset = *pos % si->block_size;

    /* 从磁盘读取数据块（扇区对齐） */
    u64 disk_offset = si->start_block + block_idx * si->block_size;
    u64 sector = SQFS_PART_START + disk_offset / 512;
    virtio_blk_read(sector, read_buf, si->block_size);

    /* 如果压缩则解压 */
    if (si->compressed) {
        squashfs_decompress(read_buf, si->block_size,
                            decomp_buf, PAGE_SIZE);
        mem_copy(buf, decomp_buf + block_offset, count);
    } else {
        mem_copy(buf, read_buf + block_offset, count);
    }

    *pos += count;
    return count;
}
```

---

## 8.4 XFS文件系统架构

XFS 使用 **Allocation Group（AG）** 并行结构，每个 AG 独立管理：

```
XFS 磁盘布局（5MB，2个AG）：

AG 0 (2.5MB)                   AG 1 (2.5MB)
┌───────────────────┐          ┌───────────────────┐
│ superblock (512B)  │          │ AG header (512B)   │
│ AGF header (512B)  │          │ AGF header (512B)  │
│ AGI header (512B)  │          │ AGI header (512B)  │
│ Free space B+tree  │          │ Free space B+tree  │
│  (bnobt root)      │          │  (bnobt root)      │
│ Inode B+tree       │          │ Inode B+tree       │
│  (inobt root)      │          │  (inobt root)      │
│ WAL Log area       │          │                    │
│ Inode blocks       │          │ Inode blocks       │
│ Data blocks        │          │ Data blocks        │
└───────────────────┘          └───────────────────┘
```

```c
/* include/linux/xfs_format.h */

#define XFS_SB_MAGIC    0x58465342  /* "XFSB"（大端序） */

/*
 * XFS 超级块（位于磁盘第0块）
 * 注意：XFS 使用大端序（big-endian），ARM64 是小端序，需要字节转换
 */
struct xfs_dsb {
    u32  sb_magicnum;    /* 0x58465342 ("XFSB")，存储为大端 */
    u32  sb_blocksize;   /* 块大小（通常4096） */
    u64  sb_dblocks;     /* 数据块总数 */
    u64  sb_logstart;    /* 日志起始块号（AG内） */
    u64  sb_rootino;     /* 根目录 inode 号 */
    u32  sb_agblocks;    /* 每个 AG 的块数 */
    u32  sb_agcount;     /* AG 数量 */
    u32  sb_logblocks;   /* 日志块数 */
    u16  sb_sectsize;    /* 扇区大小（512） */
    u16  sb_inodesize;   /* inode 大小（256字节） */
    u16  sb_inopblock;   /* 每块 inode 数 */
    u8   sb_blocklog;    /* log2(blocksize) */
    u8   sb_inodelog;    /* log2(inodesize) */
    u32  sb_icount;      /* 已分配 inode 数 */
    u32  sb_ifree;       /* 空闲 inode 数 */
    u32  sb_fdblocks;    /* 空闲数据块数 */
};

/* 大端序转换宏（ARM64 小端序环境） */
static inline u32 be32_to_cpu(u32 val) {
    return ((val & 0xff) << 24) | ((val & 0xff00) << 8) |
           ((val & 0xff0000) >> 8) | ((val >> 24) & 0xff);
}
static inline u64 be64_to_cpu(u64 val) {
    u32 hi = be32_to_cpu((u32)(val & 0xFFFFFFFF));
    u32 lo = be32_to_cpu((u32)(val >> 32));
    return ((u64)hi << 32) | lo;
}
static inline u16 be16_to_cpu(u16 val) {
    return (val >> 8) | (val << 8);
}
#define cpu_to_be32(v) be32_to_cpu(v)
#define cpu_to_be64(v) be64_to_cpu(v)
#define cpu_to_be16(v) be16_to_cpu(v)
```

## 8.5 XFS WAL日志（Write-Ahead Log）

XFS 的日志是保证崩溃一致性的核心机制：

```
写入流程：

1. 事务开始: xfs_trans_alloc()
   └── 分配 log ticket（预留日志空间）

2. 修改元数据（内存中的 inode、目录 B+树等）
   └── 将修改记录到日志项（log items）

3. 事务提交: xfs_trans_commit()
   ├── 将所有 log items 序列化为日志记录
   ├── 写入循环日志缓冲区（通过 virtio_blk_write）
   ├── 等待 I/O 完成
   └── 写入实际数据块

4. 崩溃恢复: xfs_log_recover()
   └── 重放日志中已提交但未写入数据区的事务
```

```c
/* 日志记录格式 */
#define XFS_LOG_MAGIC    0xFEEDbabe

struct xfs_log_record {
    u32  h_magicno;      /* 0xFEEDbabe（大端序） */
    u32  h_len;          /* 记录长度（包含头部，大端序） */
    u64  h_lsn;          /* Log Sequence Number（大端序） */
    u32  h_type;         /* 日志项类型 */
    u32  h_num_logops;   /* 本记录包含的操作数 */
};

/* 日志项类型 */
#define XFS_LOG_INODE_CREATE  1   /* inode 创建 */
#define XFS_LOG_INODE_UPDATE  2   /* inode 元数据更新 */
#define XFS_LOG_DIR_ADD       3   /* 目录新增条目 */
#define XFS_LOG_ALLOC         4   /* 空间分配 */
#define XFS_LOG_COMMIT        5   /* 事务提交标记 */

/* 提交一个事务到日志 */
int xfs_log_write(struct xfs_mount *mp, struct xfs_log_record *rec) {
    /* 序列化为大端序 */
    /* 写入循环日志区域（通过 virtio_blk_write） */
    u64 sector = XFS_PART_START + mp->log_start * 8 + mp->log_head;
    virtio_blk_write(sector, rec, sizeof(*rec));
    mp->log_head = (mp->log_head + 1) % mp->log_blocks;
    return 0;
}
```

## 8.6 XFS空间分配（B+树）

```c
/*
 * 每个 AG 维护 B+树管理空闲空间：
 * - bnobt：按起始块号排序（用于合并相邻空闲区）
 * - 教学简化版：单层 B+树（叶节点数组）
 *
 * 空闲区记录：
 */
struct xfs_alloc_rec {
    u32 ar_startblock;  /* AG 内起始块号 */
    u32 ar_blockcount;  /* 连续空闲块数 */
};

/*
 * B+树节点（简化版：仅叶节点，最多 128 个空闲区记录）
 */
#define XFS_BTREE_MAX_RECS  128

struct xfs_btree_block {
    u32 bb_magic;           /* 0x41425442 ("ABTB") */
    u16 bb_level;           /* 层级（0=叶节点） */
    u16 bb_numrecs;         /* 记录数 */
    struct xfs_alloc_rec recs[XFS_BTREE_MAX_RECS];
};

/*
 * 空间分配算法：
 * 1. 遍历 bnobt 叶节点找到 blockcount >= 所需大小 的最小空闲区
 * 2. 分割空闲区（如果比所需大）
 * 3. 更新 B+树
 * 4. 将分配操作写入 WAL 日志
 */
int xfs_alloc_ag_vextent(struct xfs_mount *mp, u32 agno,
                          u32 minlen, u32 *bno_out) {
    /* 读取 AG 的 bnobt */
    /* 查找第一个 size >= minlen 的记录 */
    /* 分配并更新 B+树 */
    /* 写入 WAL 日志 */
    return 0;
}
```

## 8.7 XFS 目录 B+树

```c
/*
 * XFS 目录使用 B+树组织目录项。
 * 教学简化版：小目录使用 shortform（内嵌在 inode 中）。
 *
 * 目录项格式：
 */
struct xfs_dir2_entry {
    u64 inumber;         /* inode 号 */
    u8  namelen;         /* 文件名长度 */
    char name[];         /* 文件名 */
};

/*
 * shortform 目录（小目录，条目直接存在 inode 数据区）：
 */
struct xfs_dir2_sf_hdr {
    u32 count;           /* 条目数 */
    u64 parent;          /* 父目录 inode 号 */
};
```

## 8.8 内核自建文件系统镜像

由于教学环境无法使用 `mksquashfs` / `mkfs.xfs` 外部工具，
内核在测试时自行构建文件系统镜像到磁盘分区。

### squashfs 测试镜像构建

```c
/*
 * squashfs_mkfs_test() - 在磁盘上构建最小 squashfs 镜像
 *
 * 镜像内容：
 *   /hello.txt    — "squashfs works!\n"（16字节）
 *   /readme.txt   — "read-only fs\n"（13字节）
 *
 * 构建步骤：
 *   1. 在数据区写入文件内容块
 *   2. 构建 inode table（root dir + 2 个文件 inode）
 *   3. 构建 directory table（root dir 的 2 个条目）
 *   4. 写入 superblock（包含各 table 的偏移）
 */
void squashfs_mkfs_test(void);
```

### XFS 格式化

```c
/*
 * xfs_mkfs() - 格式化 XFS 文件系统
 *
 * 步骤：
 *   1. 写入超级块（含 AG 参数）
 *   2. 写入 AG 0/1 的 AGF/AGI 头
 *   3. 初始化空闲空间 B+树（整个 AG 为一个大空闲区）
 *   4. 创建根目录 inode
 *   5. 初始化空的 WAL 日志区域
 */
void xfs_mkfs(void);
```

## 8.9 验证方法

```bash
# QEMU 测试（使用单个 8MB 磁盘）
qemu-system-aarch64 \
    -M virt,gic-version=3 -cpu cortex-a72 -m 1G \
    -kernel arm64os.elf \
    -drive file=disk.img,format=raw,if=none,id=blk0 \
    -device virtio-blk-device,drive=blk0 \
    -device virtio-net-device,netdev=net0 \
    -netdev user,id=net0 \
    -nographic
```

```c
void test_phase8(void) {
    int fd;
    char buf[64];
    ssize_t n;
    struct file *filp;

    /* === squashfs 测试 === */

    /* 1. 在磁盘上构建 squashfs 测试镜像 */
    squashfs_mkfs_test();

    /* 2. 在根 ramfs 中创建挂载点目录 */
    /* mkdir /sq */

    /* 3. 挂载 squashfs */
    do_mount("none", "/sq", "squashfs", 0, NULL);

    /* 4. 读取文件验证 */
    fd = do_sys_open(&init_files, "/sq/hello.txt", O_RDONLY, 0);
    filp = fget(&init_files, fd);
    n = vfs_read(filp, buf, 64);
    /* 验证内容 == "squashfs works!\n" */
    do_sys_close(&init_files, fd);
    printk("squashfs read: PASS\n");

    /* === XFS 测试 === */

    /* 5. 格式化 XFS */
    xfs_mkfs();

    /* 6. 挂载 XFS */
    do_mount("none", "/xfs", "xfs", 0, NULL);

    /* 7. 创建文件，验证 WAL 日志 */
    fd = do_sys_open(&init_files, "/xfs/test.txt",
                     O_CREAT | O_WRONLY, 0644);
    filp = fget(&init_files, fd);
    vfs_write(filp, "XFS WAL test\n", 13);
    do_sys_close(&init_files, fd);

    /* 8. 重新打开读取验证 */
    fd = do_sys_open(&init_files, "/xfs/test.txt", O_RDONLY, 0);
    filp = fget(&init_files, fd);
    n = vfs_read(filp, buf, 64);
    /* 验证内容 == "XFS WAL test\n" */
    do_sys_close(&init_files, fd);
    printk("XFS write+read: PASS\n");

    printk("Phase 8 complete\n");
}
```

## 8.10 本阶段产出文件

```
arm64os/
├── include/linux/
│   ├── squashfs_fs.h    ← squashfs 磁盘格式定义（新增）
│   └── xfs_format.h     ← XFS 磁盘格式定义（新增）
├── fs/
│   ├── vfs/
│   │   ├── super.c      ← 增强：多挂载点 do_mount
│   │   └── namei.c      ← 增强：挂载点穿越 path_lookup
│   ├── squashfs/
│   │   ├── super.c      ← 超级块读取、挂载、mkfs_test（核心）
│   │   ├── inode.c      ← inode 解析
│   │   ├── file.c       ← 文件数据读取
│   │   ├── dir.c        ← 目录读取
│   │   └── decompressor.c ← 解压接口（LZ4 简化实现）
│   └── xfs/
│       ├── xfs_super.c  ← XFS 挂载、AG 初始化、mkfs（核心）
│       ├── xfs_log.c    ← WAL 日志（核心）
│       ├── xfs_inode.c  ← inode 读写
│       ├── xfs_alloc.c  ← B+树空间分配
│       └── xfs_dir2.c   ← 目录 B+树
├── kernel/
│   └── main.c           ← 新增 Phase 8 初始化 + 测试
└── Makefile             ← 新增编译目标 + 8MB 磁盘 + Phase 8 测试断言
```
