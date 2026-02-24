# Phase 8 Walkthrough: squashfs 只读文件系统 + XFS 日志文件系统

> **目标**：实现两种真实磁盘文件系统 — 只读的 squashfs 和可读写的 XFS。
> **最终效果**：从 squashfs 读取文件，在 XFS 上创建/写入/读取文件，WAL 日志记录。

---

## 8.1 磁盘分区布局

```
  8MB 虚拟磁盘 (QEMU -drive 参数)
  ┌───────────────────────────────────────────────────────┐
  │ Sector 0-2047        │ 测试区域 (1MB)                 │
  ├──────────────────────┤                                │
  │ Sector 2048-6143     │ squashfs 分区 (2MB)            │
  ├──────────────────────┤                                │
  │ Sector 6144-16383    │ XFS 分区 (5MB)                 │
  └───────────────────────────────────────────────────────┘
```

Phase 8 在内核启动时自己构建这些文件系统镜像（没有 mkfs 工具），这是**自举**的一部分。

---

## 8.2 squashfs — 压缩只读文件系统

### 为什么需要 squashfs？

容器镜像（Docker image）的底层就是 squashfs。它的特点：

- **只读** — 保证镜像不被修改
- **压缩** — 节省存储空间
- **简单** — 非常适合嵌入式和容器场景

### 磁盘格式

```
  ┌────────────────┐ Block 0
  │   Super Block   │ 96 字节：魔数、块大小、inode 数等
  ├────────────────┤ Block 1
  │   Data Block 0  │ hello.txt 的数据
  ├────────────────┤ Block 2
  │   Data Block 1  │ readme.txt 的数据
  ├────────────────┤ Block 3
  │   Inode Table   │ 所有 inode（每个 48 字节）
  ├────────────────┤ Block 4
  │   Dir Table     │ 目录项
  └────────────────┘
```

### Super Block

```c
struct squashfs_super_block {
    uint32_t s_magic;           /* 0x73717368 = "sqsh" */
    uint32_t inodes;            /* inode 数量 */
    uint32_t bytes_used;        /* 总大小 */
    uint32_t block_size;        /* 块大小（4KB） */
    uint16_t compression;       /* 压缩算法（NONE=0） */
    uint16_t flags;
    uint32_t inode_table_start; /* inode 表起始块 */
    uint32_t directory_table_start; /* 目录表起始块 */
    uint32_t root_inode;        /* 根 inode 号 */
};
```

### 构建测试镜像

```c
void squashfs_mkfs_test(void)
{
    /* 数据 */
    char hello_data[] = "Hello from squashfs!\n";
    char readme_data[] = "This is a read-only file.\n";

    /* Block 0: Super Block */
    struct squashfs_super_block sb;
    sb.s_magic = SQUASHFS_MAGIC;
    sb.inodes = 3;              /* 根目录 + 2 个文件 */
    sb.block_size = 4096;
    sb.compression = SQUASHFS_COMP_NONE;
    sb.inode_table_start = 3;   /* Block 3 */
    sb.directory_table_start = 4; /* Block 4 */
    sb.root_inode = 1;          /* 根目录 = inode 1 */
    virtio_blk_write(squashfs_start_sector, &sb, 512);

    /* Block 1-2: 数据块 */
    virtio_blk_write(squashfs_start_sector + 8, hello_data, 512);
    virtio_blk_write(squashfs_start_sector + 16, readme_data, 512);

    /* Block 3: Inode 表 */
    struct squashfs_inode inodes[3];
    inodes[0] = { .type = SQUASHFS_DIR_TYPE, .mode = 0755, ... };
    inodes[1] = { .type = SQUASHFS_REG_TYPE, .start_block = 1,
                  .file_size = 21, ... };
    inodes[2] = { .type = SQUASHFS_REG_TYPE, .start_block = 2,
                  .file_size = 26, ... };
    virtio_blk_write(..., inodes, sizeof(inodes));

    /* Block 4: 目录表 */
    /* entry: inode_number(8) + type(1) + name_size(1) + name */
}
```

### squashfs 读取路径

```c
static ssize_t squashfs_read(struct file *filp, char *buf,
                             size_t count, loff_t *pos)
{
    struct squashfs_inode_info *info = filp->f_inode->i_private;

    /* 计算数据块位置 */
    unsigned long block = info->start_block;
    unsigned long disk_sector = squashfs_partition_start + block * 8;

    /* 从磁盘读取 */
    char tmp[4096];
    virtio_blk_read(disk_sector, tmp, 4096);

    /* 如果有压缩，解压 */
    /* squashfs_decompress(tmp, out, comp_type); */

    /* 拷贝到用户缓冲区 */
    size_t to_copy = min(count, (size_t)(info->file_size - *pos));
    memcpy(buf, tmp + *pos, to_copy);
    *pos += to_copy;

    return to_copy;
}
```

---

## 8.3 XFS — 日志文件系统

### 为什么需要 XFS？

XFS 是 Linux 上高性能文件系统，特点：

- **WAL 日志** — 崩溃一致性（Write-Ahead Logging）
- **B+ 树** — 高效空间管理
- **大端存储** — 磁盘格式使用 big-endian

### XFS 分区布局

```
  5MB XFS 分区，分成 2 个 AG（Allocation Group）:

  AG 0 (2.5MB):
  ┌──────────┬──────┬──────┬────────┬────────┬─────────┬──────┬──────┐
  │Super Block│ AGF  │ AGI  │ bnobt  │ inobt  │ WAL Log │Inodes│ Data │
  │  Block 0  │  1   │  2   │  3     │  4     │ 5-36    │  37  │ 38+  │
  └──────────┴──────┴──────┴────────┴────────┴─────────┴──────┴──────┘

  AG 1 (2.5MB):
  ┌──────────┬──────┬──────┬────────┬────────┬──────┬──────┐
  │Super Block│ AGF  │ AGI  │ bnobt  │ inobt  │Inodes│ Data │
  │  Block 0  │  1   │  2   │  3     │  4     │  5   │ 6+   │
  └──────────┴──────┴──────┴────────┴────────┴──────┴──────┘
```

### XFS Super Block（大端！）

```c
struct xfs_dsb {
    uint32_t sb_magicnum;       /* 0x58465342 = "XFSB" (big-endian) */
    uint32_t sb_blocksize;      /* 块大小（4096） */
    uint64_t sb_dblocks;        /* 总块数 */
    uint32_t sb_agcount;        /* AG 数量 */
    uint32_t sb_agblocks;       /* 每 AG 块数 */
    uint64_t sb_rootino;        /* 根目录 inode 号 */
    /* ... */
};

/* 大端转换（ARM64 是小端） */
static inline uint32_t be32_to_cpu(uint32_t be_val)
{
    return __builtin_bswap32(be_val);
}
```

### WAL 日志 — 崩溃安全的秘密

**原理**：先把要做的修改写入日志，再修改实际数据。崩溃后通过重放日志恢复。

```
  正常写入流程:

  1. 开始事务
  2. 修改元数据（内存中）
  3. 写日志记录 ──────────► WAL 区域（磁盘）
  4. 写实际数据 ──────────► 数据区域（磁盘）
  5. 提交事务

  崩溃恢复:
  - 如果步骤 3 完成但步骤 4 未完成 → 重放日志
  - 如果步骤 3 未完成 → 忽略不完整的日志记录
```

```c
struct xfs_log_record {
    uint32_t lr_magic;     /* 0xFEEDbabe */
    uint32_t lr_len;       /* 记录长度 */
    uint64_t lr_lsn;       /* 日志序列号（单调递增） */
    uint32_t lr_type;      /* LOG_INODE_CREATE, LOG_INODE_WRITE, ... */
    uint32_t lr_num_logops; /* 操作数 */
};

int xfs_log_write_record(struct xfs_mount *mp, int type, ...)
{
    struct xfs_log_record rec;
    rec.lr_magic = cpu_to_be32(XFS_LOG_MAGIC);
    rec.lr_lsn = cpu_to_be64(mp->log_lsn++);
    rec.lr_type = cpu_to_be32(type);

    /* 写入日志区域 */
    virtio_blk_write(log_sector, &rec, sizeof(rec));

    return 0;
}
```

### XFS 文件创建

```c
int xfs_create_file(struct inode *dir, struct dentry *dentry, unsigned int mode)
{
    /* Step 1: 分配 inode 号 */
    unsigned long ino = xfs_alloc_inode_number(mp);

    /* Step 2: 分配数据块 */
    unsigned long data_block = xfs_alloc_block(mp, 0);

    /* Step 3: 写 WAL 日志 */
    xfs_log_write_record(mp, LOG_INODE_CREATE, ino);

    /* Step 4: 初始化 inode（磁盘格式，大端） */
    struct xfs_dinode dinode;
    dinode.di_magic = cpu_to_be16(XFS_DINODE_MAGIC);
    dinode.di_mode = cpu_to_be16(mode);
    dinode.di_size = 0;
    dinode.di_format = XFS_DINODE_FMT_EXTENTS;
    /* 写入磁盘 */
    xfs_inode_write(mp, ino, &dinode);

    /* Step 5: 添加目录项 */
    xfs_dir_add_entry(dir, dentry->d_name.name, ino);

    /* Step 6: 创建 VFS inode */
    struct inode *inode = xfs_iget(dir->i_sb, ino);
    d_add(dentry, inode);

    return 0;
}
```

### 空间分配（简化版）

```c
/* 每个 AG 的空闲空间追踪 */
struct ag_free_space {
    unsigned long start_block;
    unsigned long free_count;
};

unsigned long xfs_alloc_block(struct xfs_mount *mp, int ag_no)
{
    struct ag_free_space *ag = &mp->ag_free[ag_no];

    if (ag->free_count == 0)
        return 0;  /* AG 满 */

    unsigned long block = ag->start_block;
    ag->start_block++;
    ag->free_count--;

    return block;
}
```

---

## 8.4 挂载到 VFS

```c
/* 在 start_kernel() 中 */

/* 注册文件系统 */
squashfs_init();  /* register_filesystem("squashfs") */
xfs_init();       /* register_filesystem("xfs") */

/* 构建镜像 */
squashfs_mkfs_test();  /* 写入 squashfs 镜像到磁盘 */
xfs_mkfs();            /* 格式化 XFS 分区 */

/* 挂载 */
do_mount("none", "/sq", "squashfs", 0, NULL);
do_mount("none", "/xfs", "xfs", 0, NULL);
```

挂载后的目录树：

```
  /               ← ramfs (Phase 7)
  ├── test.txt
  ├── subdir/
  ├── sq/         ← squashfs (只读)
  │   ├── hello.txt    "Hello from squashfs!\n"
  │   └── readme.txt   "This is a read-only file.\n"
  └── xfs/        ← XFS (可读写)
      └── (空，等待测试创建文件)
```

---

## 8.5 测试验证

```c
static void test_phase8(void)
{
    /* squashfs 测试：读取只读文件 */
    int fd = do_sys_open(&init_files, "/sq/hello.txt", O_RDONLY, 0);
    char buf[64];
    ssize_t n = vfs_read(fget(&init_files, fd), buf, 64);
    /* 验证内容 == "Hello from squashfs!\n" */

    /* XFS 测试：创建并写入文件 */
    fd = do_sys_open(&init_files, "/xfs/data.txt", O_CREAT|O_WRONLY, 0644);
    vfs_write(fget(&init_files, fd), "xfs works!\n", 11);
    do_sys_close(&init_files, fd);

    /* XFS 测试：重新读取验证 */
    fd = do_sys_open(&init_files, "/xfs/data.txt", O_RDONLY, 0);
    n = vfs_read(fget(&init_files, fd), buf, 64);
    /* 验证内容 == "xfs works!\n" */
}
```

---

## 8.6 Phase 8 核心概念总结

| 概念 | 说明 |
|------|------|
| **squashfs** | 只读压缩文件系统，容器镜像底层 |
| **XFS** | 高性能日志文件系统，大端磁盘格式 |
| **AG** | Allocation Group，XFS 空间管理单元 |
| **WAL 日志** | Write-Ahead Logging，崩溃一致性保证 |
| **be32_to_cpu** | 大端↔小端转换 |
| **自举镜像** | 内核启动时自己构建文件系统镜像 |
| **挂载表** | 支持多个文件系统挂载到不同路径 |

**Phase 8 奠定的基础**：有了只读层（squashfs）和可读写层（ramfs/XFS），Phase 9 的 overlayfs 可以将它们联合挂载。
