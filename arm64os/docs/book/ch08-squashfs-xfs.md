# Phase 8：squashfs 只读层 + XFS 日志文件系统

## 知识来源总览

- **磁盘格式规范**：约 50%（squashfs/XFS superblock、B+树节点、日志记录）
- **文件系统理论**：约 20%（WAL 日志、B+树、环形缓冲区）
- **Linux VFS 接口**：约 15%
- **前序 Phase 依赖**：约 10%
- **ARM64（字节序转换）**：约 5%

## squashfs 磁盘格式

```c
struct squashfs_super_block {
    __le32 s_magic;       /* 0x73717368 ("sqsh")，小端 */
    __le32 inodes;        /* inode 总数 */
    __le32 block_size;    /* 数据块大小，默认 128KB */
    __le16 compression;   /* 1=zlib,4=xz,5=lz4,6=zstd */
    __le64 root_inode;    /* 根 inode 偏移 */
    __le64 inode_table_start;
    __le64 directory_table_start;
};
```

**block_size = 128KB**：压缩率与随机访问延迟的权衡。块越大压缩率越好，但读取一个字节需要解压更多数据。

**compression 字段**：挂载时选择解压器。现代容器镜像首选 zstd（编号 6）——压缩率接近 xz，解压速度接近 lz4。

### squashfs 文件读取

```c
int squashfs_readpage(struct file *file, struct page *page) {
    int block_idx = page_offset(page) >> sb->s_blocksize_bits;
    u64 block_offset = ei->block_list[block_idx];
    bool compressed = !(block_size & SQUASHFS_COMPRESSED_BIT);

    void *buf = kmalloc(block_size);
    block_device_read(sb->s_bdev, block_offset, buf, block_size);

    if (compressed)
        squashfs_decompress(sb, buf, block_size,
                           page_address(page), PAGE_SIZE);
    else
        memcpy(page_address(page), buf, PAGE_SIZE);

    SetPageUptodate(page);
    kfree(buf);
    return 0;
}
```

**SQUASHFS_COMPRESSED_BIT**：如果数据压缩后比原始还大，squashfs 存储原始数据并设置此位。逻辑取反 `!` 是因为"设位 = 未压缩"（历史兼容原因）。

## XFS 架构

### AG（Allocation Group）并行设计

```
4GB 磁盘，4 个 AG，每个 1GB：
AG 0: [superblock][AG free space B+树][inode B+树][日志][数据...]
AG 1: [AG header ][AG free space B+树][inode B+树][数据...]
AG 2: [AG header ][AG free space B+树][inode B+树][数据...]
AG 3: [AG header ][AG free space B+树][inode B+树][数据...]
```

每个 AG 独立管理空间和 inode，有独立的锁。线程 A 写 AG0，线程 B 写 AG2，互不竞争。

### XFS superblock（大端！）

```c
struct xfs_dsb {
    __be32  sb_magicnum;    /* 0x58465342 ("XFSB")，大端 */
    __be32  sb_blocksize;   /* 块大小（通常 4096）*/
    __be64  sb_dblocks;     /* 数据块总数 */
    __be32  sb_agblocks;    /* 每个 AG 的块数 */
    __be32  sb_agcount;     /* AG 数量 */
    __be16  sb_inodesize;   /* inode 大小（通常 512）*/
    __be8   sb_blocklog;    /* log2(blocksize) = 12 */
    __be8   sb_agblklog;    /* log2(agblocks) */
};
```

**`__be32` = 大端**。XFS 由 SGI 在 MIPS 大端机器上开发（1993），磁盘格式固定大端。ARM64 上需要 `be32_to_cpu()` 转换，编译为 `rev w0, w0`（一条指令）。

### WAL 日志

```c
struct xfs_log_record {
    __be32  h_magicno;    /* 0xFEEDbabe */
    __be16  h_cycle;      /* 循环计数（区分新旧数据）*/
    __be64  h_lsn;        /* Log Sequence Number */
    __be64  h_tail_lsn;   /* 最旧活跃日志 */
};
```

XFS 日志是环形缓冲区。`h_cycle` 每写满一圈加 1，恢复时区分新数据和旧数据。LSN 用于判断元数据块是否需要重放。

### 双 B+树空间分配

每个 AG 维护两棵 B+树：
- **bnobt**：按起始块号排序（释放时合并相邻空闲区）
- **cntbt**：按大小排序（分配时快速找足够大的空闲区）

分配 N 块：在 cntbt 中 O(log n) 找到 ≥ N 的最小区段。释放时在 bnobt 中 O(log n) 找相邻区段合并。
