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

## 8.1 squashfs磁盘格式

squashfs 是一种压缩只读文件系统，广泛用于容器镜像的只读层。

```
squashfs磁盘布局：
┌─────────────────────────────────────┐
│  superblock（96字节）               │ 偏移 0
├─────────────────────────────────────┤
│  压缩数据块（Data Blocks）           │ 文件内容，按块压缩
│  每块默认 128KB，独立压缩            │
├─────────────────────────────────────┤
│  Fragment Table（碎片表）            │ 小文件尾部合并存储
├─────────────────────────────────────┤
│  Inode Table（压缩）                │ inode元数据
├─────────────────────────────────────┤
│  Directory Table（压缩）            │ 目录项
├─────────────────────────────────────┤
│  Fragment Table Index               │ 指向Fragment Table
│  Export Table                       │ NFS导出（可选）
│  UID/GID Lookup Table               │
│  Xattr Table                        │
└─────────────────────────────────────┘
```

```c
/* 参考 include/linux/squashfs_fs.h */
struct squashfs_super_block {
    __le32 s_magic;             /* 0x73717368 ("sqsh") */
    __le32 inodes;              /* inode总数 */
    __le32 mkfs_time;           /* 创建时间戳 */
    __le32 block_size;          /* 数据块大小（默认131072=128KB）*/
    __le32 fragments;           /* 碎片块数量 */
    __le16 compression;         /* 压缩算法：1=zlib,2=lzma,3=lzo,4=xz,5=lz4,6=zstd */
    __le16 block_log;           /* block_size = 2^block_log */
    __le16 flags;               /* SQUASHFS_NOI=0x1(无inode压缩) 等 */
    __le16 no_ids;              /* UID/GID表项数 */
    __le16 s_major;             /* 版本主号（4）*/
    __le16 s_minor;             /* 版本次号（0）*/
    __le64 root_inode;          /* 根目录inode的元数据偏移 */
    __le64 bytes_used;          /* 文件系统总大小 */
    __le64 id_table_start;
    __le64 xattr_id_table_start;
    __le64 inode_table_start;
    __le64 directory_table_start;
    __le64 fragment_table_start;
    __le64 lookup_table_start;
};
```

## 8.2 squashfs数据读取流程

```c
/* 参考 fs/squashfs/file.c */
/*
 * squashfs 文件读取关键路径：
 * page_readpage() → squashfs_readpage() → squashfs_read_data()
 * → 读取压缩块 → 解压 → 填充 page cache
 */

int squashfs_readpage(struct file *file, struct page *page) {
    struct inode *inode = page->mapping->host;
    struct squashfs_inode_info *ei = squashfs_inode(inode);
    loff_t byte_index = page_offset(page);

    /* 计算该页属于哪个数据块 */
    int block_idx = byte_index >> inode->i_sb->s_blocksize_bits;

    /* 从 inode 的 block_list 中找到对应块的磁盘偏移和压缩大小 */
    u64  block_offset = ei->block_list[block_idx];
    int  block_size   = ei->block_size_list[block_idx];
    bool compressed   = !(block_size & SQUASHFS_COMPRESSED_BIT);

    /* 读取压缩数据 */
    void *compressed_buf = kmalloc(block_size & ~SQUASHFS_COMPRESSED_BIT);
    block_device_read(inode->i_sb->s_bdev, block_offset, compressed_buf,
                      block_size & ~SQUASHFS_COMPRESSED_BIT);

    if (compressed) {
        /* 解压到目标页 */
        squashfs_decompress(inode->i_sb, compressed_buf,
                            block_size & ~SQUASHFS_COMPRESSED_BIT,
                            page_address(page), PAGE_SIZE);
    } else {
        /* 未压缩块直接拷贝 */
        memcpy(page_address(page), compressed_buf, PAGE_SIZE);
    }

    SetPageUptodate(page);
    kfree(compressed_buf);
    return 0;
}
```

---

## 8.3 XFS文件系统架构

XFS 使用 **Allocation Group（AG）** 并行结构，每个 AG 独立管理：

```
XFS 磁盘布局（以 4GB 磁盘为例，4个AG）：

AG 0                AG 1                AG 2                AG 3
┌───────────────┐  ┌───────────────┐  ┌───────────────┐  ┌───────────────┐
│ superblock    │  │ AG header     │  │ AG header     │  │ AG header     │
│ AG free space │  │ AG free space │  │ AG free space │  │ AG free space │
│ B+树 (bno/cnt)│  │ B+树          │  │ B+树          │  │ B+树          │
│ inode B+树    │  │ inode B+树    │  │ inode B+树    │  │ inode B+树    │
│ Log (WAL)     │  │               │  │               │  │               │
│ 数据块...     │  │ 数据块...     │  │ 数据块...     │  │ 数据块...     │
└───────────────┘  └───────────────┘  └───────────────┘  └───────────────┘
```

```c
/* 参考 fs/xfs/libxfs/xfs_format.h */
/* XFS超级块（位于磁盘第0块）*/
struct xfs_dsb {
    __be32  sb_magicnum;    /* 0x58465342 ("XFSB") */
    __be32  sb_blocksize;   /* 块大小（通常4096）*/
    __be64  sb_dblocks;     /* 数据块总数 */
    __be64  sb_rblocks;     /* 实时区块数（通常0）*/
    __be64  sb_rextents;
    uuid_t  sb_uuid;
    __be64  sb_logstart;    /* 日志起始块（AG内日志）*/
    __be64  sb_rootino;     /* 根目录inode号 */
    __be64  sb_rbmino;      /* 实时位图inode */
    __be64  sb_rsumino;     /* 实时摘要inode */
    __be32  sb_rextsize;
    __be32  sb_agblocks;    /* 每个AG的块数 */
    __be32  sb_agcount;     /* AG数量 */
    __be32  sb_rbmblocks;
    __be32  sb_logblocks;   /* 日志块数 */
    __be16  sb_versionnum;  /* 版本（含特性位）*/
    __be16  sb_sectsize;    /* 扇区大小 */
    __be16  sb_inodesize;   /* inode大小（通常512字节）*/
    __be16  sb_inopblock;   /* 每块inode数 */
    char    sb_fname[12];   /* 文件系统名称 */
    __be8   sb_blocklog;    /* log2(blocksize) */
    __be8   sb_sectlog;
    __be8   sb_inodelog;
    __be8   sb_inopblog;
    __be8   sb_agblklog;    /* log2(agblocks) */
    /* ... */
};
```

## 8.4 XFS WAL日志（Write-Ahead Log）

XFS 的日志是保证崩溃一致性的核心机制：

```
写入流程（参考 fs/xfs/xfs_log.c）：

1. 事务开始: xfs_trans_alloc()
   └── 分配 log ticket（预留日志空间）

2. 修改元数据（内存中的 AG 结构、inode、目录 B+树）
   └── 将修改标记到日志项（log items）

3. 事务提交: xfs_trans_commit()
   ├── 将所有 log items 序列化为日志记录
   ├── 写入循环日志缓冲区（log buffer）
   ├── 等待 I/O 完成（fsync路径）或异步刷新
   └── 完成后才写入实际数据块

4. 崩溃恢复: xfs_log_recover()
   └── 重放日志中已提交但未写入数据区的事务
```

```c
/* 参考 fs/xfs/xfs_log.c */

/* 日志记录格式 */
struct xfs_log_record {
    __be32  h_magicno;      /* 0xFEEDbabe */
    __be16  h_cycle;        /* 循环计数（区分日志覆盖）*/
    __be16  h_version;      /* 版本（2）*/
    __be32  h_len;          /* 记录长度（包含头部）*/
    __be64  h_lsn;          /* Log Sequence Number */
    __be64  h_tail_lsn;     /* 最旧的活跃日志 LSN */
    __le32  h_crc;          /* CRC32校验 */
    __be32  h_prev_block;   /* 前一个日志块号 */
    __be32  h_num_logops;   /* 本记录包含的操作数 */
    __be32  h_cycle_data[]; /* 循环数据 */
};

/* 提交一个事务到日志 */
int xfs_log_commit(struct xfs_mount *mp, struct xfs_log_vec *log_vector,
                   struct xlog_ticket *ticket, xfs_lsn_t *commit_lsn) {
    struct xlog *log = mp->m_log;

    /* 1. 将 log_vector 中的所有修改写入日志缓冲区 */
    xlog_write(log, log_vector, ticket, commit_lsn, XLOG_COMMIT_TRANS);

    /* 2. 触发日志 I/O（写入磁盘）*/
    xlog_state_finish_copy(log, iclog, ticket->t_curr_res);

    return 0;
}
```

## 8.5 XFS空间分配（B+树）

```c
/* 参考 fs/xfs/libxfs/xfs_alloc.c */
/*
 * 每个 AG 维护两棵 B+树管理空闲空间：
 * - bnobt：按起始块号排序（用于合并相邻空闲区）
 * - cntbt：按大小排序（用于快速找到足够大的空闲区）
 */

int xfs_alloc_ag_vextent(struct xfs_alloc_arg *args) {
    /* 在 cntbt 中找到大小 >= args->minlen 的最小空闲区 */
    xfs_btree_cursor_t *cnt_cur = xfs_allocbt_init_cursor(
        args->mp, args->tp, args->agbp, args->agno, XFS_BTNUM_CNT);

    /* B+树查找：找到第一个 size >= minlen 的记录 */
    xfs_alloc_lookup_ge(cnt_cur, 0, args->minlen, &cnt_stat);

    /* 从 bnobt 中找到具体位置并分配 */
    xfs_btree_cursor_t *bno_cur = xfs_allocbt_init_cursor(
        args->mp, args->tp, args->agbp, args->agno, XFS_BTNUM_BNO);

    /* 更新两棵B+树（删除已分配的空闲区记录）*/
    xfs_btree_delete(cnt_cur);
    xfs_btree_delete(bno_cur);

    args->agbno = bno;   /* 返回分配的AG内块号 */
    return 0;
}
```

## 8.6 验证方法

```bash
# 创建 squashfs 镜像
mksquashfs rootfs/ rootfs.sqsh -comp zstd

# 创建 XFS 文件系统镜像（在 virtio 块设备上）
mkfs.xfs -b size=4096 /dev/vda

# QEMU 测试
qemu-system-aarch64 \
    -M virt -cpu cortex-a72 -m 1G \
    -kernel arm64os.elf \
    -drive file=rootfs.sqsh,format=raw,if=none,id=sqsh \
    -device virtio-blk-device,drive=sqsh \
    -drive file=xfs.img,format=raw,if=none,id=xfs \
    -device virtio-blk-device,drive=xfs \
    -nographic
```

```c
void test_filesystems(void) {
    /* 挂载 squashfs（只读）*/
    do_mount("/dev/vda", "/ro", "squashfs", MS_RDONLY, NULL);

    /* 列出根目录 */
    struct file *f = filp_open("/ro/", O_RDONLY | O_DIRECTORY, 0);
    iterate_dir(f, &readdir_ctx);
    printk("squashfs mount OK\n");

    /* 挂载 XFS（读写）*/
    do_mount("/dev/vdb", "/rw", "xfs", 0, NULL);

    /* 创建文件，验证日志持久化 */
    int fd = sys_openat(AT_FDCWD, "/rw/test.txt", O_CREAT|O_WRONLY, 0644);
    sys_write(fd, "XFS WAL test\n", 13);
    sys_fsync(fd);  /* 强制日志落盘 */
    sys_close(fd);
    printk("XFS write + fsync OK\n");
}
```

## 8.7 本阶段产出文件

```
arm64os/
└── fs/
    ├── squashfs/
    │   ├── super.c          ← 超级块读取与挂载（核心）
    │   ├── inode.c          ← inode解析
    │   ├── file.c           ← 文件数据读取
    │   ├── dir.c            ← 目录读取
    │   └── decompressor.c   ← zstd/lz4解压接口
    └── xfs/
        ├── xfs_super.c      ← XFS挂载与AG初始化（核心）
        ├── xfs_log.c        ← WAL日志（核心）
        ├── xfs_inode.c      ← inode读写
        ├── xfs_alloc.c      ← B+树空间分配
        └── xfs_dir2.c       ← 目录B+树
```
