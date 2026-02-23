/* SPDX-License-Identifier: GPL-2.0 */
/*
 * arm64os/fs/squashfs/super.c
 *
 * squashfs 超级块读取、挂载、测试镜像构建
 *
 * 参考：fs/squashfs/super.c
 *
 * Phase 8 教学简化版：
 *   - 从 VirtIO 块设备分区读取 squashfs 超级块
 *   - 注册 squashfs 文件系统类型
 *   - squashfs_mkfs_test() 在磁盘上构建最小测试镜像
 */

#include <linux/types.h>
#include <linux/fs.h>
#include <linux/squashfs_fs.h>

/* 外部函数 */
void boot_printk(const char *s);
void boot_printk_hex(unsigned long val);
int virtio_blk_read(u64 sector, void *buf, u32 len);
int virtio_blk_write(u64 sector, const void *buf, u32 len);
struct super_block *alloc_super(struct file_system_type *type);
struct inode *squashfs_iget(struct super_block *sb, u32 inode_number);
struct dentry *d_alloc_root(struct super_block *sb);
int register_filesystem(struct file_system_type *fs);

/*
 * ============================================================
 * 内部辅助函数
 * ============================================================
 */
static void mem_zero(void *dst, u32 len)
{
    u8 *p = (u8 *)dst;
    u32 i;
    for (i = 0; i < len; i++)
        p[i] = 0;
}

static void mem_copy(void *dst, const void *src, u32 len)
{
    u8 *d = (u8 *)dst;
    const u8 *s = (const u8 *)src;
    u32 i;
    for (i = 0; i < len; i++)
        d[i] = s[i];
}

/* squashfs_sb_info 静态池 */
static struct squashfs_sb_info sqfs_sb_pool[2];
static int sqfs_sb_idx = 0;

/*
 * ============================================================
 * squashfs_fill_super - 从磁盘读取超级块并填充 VFS super_block
 *
 * @sb: 已分配的 VFS 超级块
 *
 * 返回 0 成功，负数失败。
 *
 * 参考：fs/squashfs/super.c squashfs_fill_super()
 * ============================================================
 */
static int squashfs_fill_super(struct super_block *sb)
{
    struct squashfs_super_block dsb;
    struct squashfs_sb_info *sbi;
    struct inode *root_inode;
    struct dentry *root_dentry;
    static u8 __attribute__((aligned(4096))) io_buf[512];

    /* 从分区起始读取超级块 */
    if (virtio_blk_read(SQFS_PART_START, io_buf, 512) != 0) {
        boot_printk("[squashfs] ERROR: read superblock failed\n");
        return -1;
    }

    mem_copy(&dsb, io_buf, sizeof(dsb));

    /* 验证魔数 */
    if (dsb.s_magic != SQUASHFS_MAGIC) {
        boot_printk("[squashfs] ERROR: bad magic ");
        boot_printk_hex(dsb.s_magic);
        boot_printk(" (expected ");
        boot_printk_hex(SQUASHFS_MAGIC);
        boot_printk(")\n");
        return -1;
    }

    boot_printk("[squashfs] superblock: magic=");
    boot_printk_hex(dsb.s_magic);
    boot_printk(" inodes=");
    boot_printk_hex(dsb.inodes);
    boot_printk(" block_size=");
    boot_printk_hex(dsb.block_size);
    boot_printk("\n");

    /* 分配 squashfs 私有数据 */
    if (sqfs_sb_idx >= 2) {
        boot_printk("[squashfs] ERROR: sb info pool exhausted\n");
        return -1;
    }
    sbi = &sqfs_sb_pool[sqfs_sb_idx++];

    sbi->block_size = dsb.block_size;
    sbi->block_log = dsb.block_log;
    sbi->compression = dsb.compression;
    sbi->inodes = dsb.inodes;
    sbi->inode_table_start = dsb.inode_table_start;
    sbi->dir_table_start = dsb.directory_table_start;
    sbi->root_inode_offset = dsb.root_inode;

    /* 填充 VFS super_block */
    sb->s_magic = SQUASHFS_MAGIC;
    sb->s_blocksize = dsb.block_size;
    sb->s_fs_info = sbi;

    /* 读取根 inode（根 inode 编号 = root_inode_offset 指向的位置） */
    /*
     * 我们的简化版中，root_inode 字段直接存储根 inode 编号。
     * mkfs_test 将 root_inode = 1（根目录 inode 编号 = 1）。
     */
    root_inode = squashfs_iget(sb, (u32)sbi->root_inode_offset);
    if (!root_inode) {
        boot_printk("[squashfs] ERROR: read root inode failed\n");
        return -1;
    }

    /* 创建根 dentry */
    root_dentry = d_alloc_root(sb);
    if (!root_dentry) {
        boot_printk("[squashfs] ERROR: create root dentry failed\n");
        return -1;
    }
    root_dentry->d_inode = root_inode;
    sb->s_root = root_dentry;

    boot_printk("[squashfs] mounted: ");
    boot_printk_hex(dsb.inodes);
    boot_printk(" inodes, root_ino=");
    boot_printk_hex((unsigned long)sbi->root_inode_offset);
    boot_printk("\n");

    return 0;
}

/*
 * ============================================================
 * squashfs_mount - 文件系统 mount 回调
 *
 * 参考：fs/squashfs/super.c squashfs_mount()
 * ============================================================
 */
static struct dentry *squashfs_mount(struct file_system_type *fs_type,
                                      int flags, const char *dev_name,
                                      void *data)
{
    struct super_block *sb;
    int ret;

    boot_printk("[squashfs] Mounting squashfs...\n");

    sb = alloc_super(fs_type);
    if (!sb)
        return NULL;

    ret = squashfs_fill_super(sb);
    if (ret != 0)
        return NULL;

    return sb->s_root;
}

/*
 * ============================================================
 * squashfs_kill_sb - 卸载回调（简化版不回收资源）
 * ============================================================
 */
static void squashfs_kill_sb(struct super_block *sb)
{
    boot_printk("[squashfs] Unmounting squashfs\n");
    (void)sb;
}

/*
 * ============================================================
 * 文件系统类型注册
 * ============================================================
 */
static struct file_system_type squashfs_fs_type = {
    .name    = "squashfs",
    .mount   = squashfs_mount,
    .kill_sb = squashfs_kill_sb,
    .next    = NULL,
};

/*
 * ============================================================
 * squashfs_init - 注册 squashfs 文件系统
 *
 * 在 VFS 初始化后调用。
 *
 * 参考：fs/squashfs/super.c init_squashfs_fs()
 * ============================================================
 */
void squashfs_init(void)
{
    register_filesystem(&squashfs_fs_type);
}

/*
 * ============================================================
 * squashfs_mkfs_test - 在磁盘上构建最小 squashfs 测试镜像
 *
 * 镜像内容：
 *   /hello.txt    — "squashfs works!\n"（16字节）
 *   /readme.txt   — "read-only fs\n"（13字节）
 *
 * 磁盘布局（相对分区起始）：
 *   扇区 0:      superblock（96字节 + 零填充到 512 字节）
 *   扇区 1-8:    数据块（block 0: hello.txt 4KB, block 1: readme.txt 4KB）
 *   扇区 9-10:   inode table（3 个 inode × 48 字节 = 144 字节）
 *   扇区 11-12:  directory table（目录项）
 *
 * 参考：Phase 8 设计文档 §8.8
 * ============================================================
 */
void squashfs_mkfs_test(void)
{
    static u8 __attribute__((aligned(4096))) buf[4096];
    struct squashfs_super_block *dsb;
    struct squashfs_inode *ino;
    struct squashfs_dir_entry *de;
    u32 data_start;
    u32 inode_table_start;
    u32 dir_table_start;

    boot_printk("[squashfs] Building test image...\n");

    /*
     * Layout (byte offsets relative to partition start):
     *   0x0000: superblock (512 bytes, padded)
     *   0x0200: data block 0 — hello.txt content (512 bytes)
     *   0x0400: data block 1 — readme.txt content (512 bytes)
     *   0x0600: inode table (3 × 48 = 144 bytes, in 512-byte sector)
     *   0x0800: directory table
     *
     * Note: we use 512-byte aligned blocks for simplicity
     * (block_size is still 4096 in the superblock for compatibility,
     *  but actual data is stored compactly).
     *
     * Actually let's use simpler byte offsets:
     *   data_start      = 512 (byte offset of first data block)
     *   inode_table_start = 1536 (512 + 512 + 512 = byte offset of inode table)
     *   dir_table_start   = 2048 (byte offset of directory table)
     */
    data_start = 512;           /* right after superblock */
    inode_table_start = 1536;   /* after 2 data sectors */
    dir_table_start = 2048;     /* after inode table */

    /* ---- Step 1: Write data blocks ---- */

    /* Data block 0: hello.txt content */
    mem_zero(buf, 512);
    mem_copy(buf, "squashfs works!\n", 16);
    virtio_blk_write(SQFS_PART_START + data_start / 512, buf, 512);

    /* Data block 1: readme.txt content */
    mem_zero(buf, 512);
    mem_copy(buf, "read-only fs\n", 13);
    virtio_blk_write(SQFS_PART_START + (data_start + 512) / 512, buf, 512);

    /* ---- Step 2: Write inode table ---- */
    /*
     * 3 inodes:
     *   inode 1: root directory (type=DIR)
     *   inode 2: hello.txt (type=REG, 16 bytes)
     *   inode 3: readme.txt (type=REG, 13 bytes)
     */
    mem_zero(buf, 512);

    /* inode 1: root directory */
    ino = (struct squashfs_inode *)(buf + 0);
    ino->inode_type = SQUASHFS_DIR_TYPE;
    ino->mode = 0755;
    ino->inode_number = 1;
    ino->file_size = 0;         /* dir: size not meaningful here */
    ino->parent_inode = 0;      /* root has no parent */
    ino->start_block = 0;
    ino->block_count = 0;
    ino->dir_offset = 0;        /* offset within dir table */
    /*
     * dir_size: total bytes of directory entries.
     * 2 entries:
     *   entry 1: "hello.txt"  — 8 + 9 = 17 bytes
     *   entry 2: "readme.txt" — 8 + 10 = 18 bytes
     *   total = 35 bytes
     */
    ino->dir_size = 35;

    /* inode 2: hello.txt */
    ino = (struct squashfs_inode *)(buf + 48);
    ino->inode_type = SQUASHFS_REG_TYPE;
    ino->mode = 0644;
    ino->inode_number = 2;
    ino->file_size = 16;        /* "squashfs works!\n" */
    ino->parent_inode = 1;
    ino->start_block = data_start;  /* byte offset from partition start */
    ino->block_count = 1;
    ino->dir_offset = 0;
    ino->dir_size = 0;

    /* inode 3: readme.txt */
    ino = (struct squashfs_inode *)(buf + 96);
    ino->inode_type = SQUASHFS_REG_TYPE;
    ino->mode = 0644;
    ino->inode_number = 3;
    ino->file_size = 13;        /* "read-only fs\n" */
    ino->parent_inode = 1;
    ino->start_block = data_start + 512;  /* second data block */
    ino->block_count = 1;
    ino->dir_offset = 0;
    ino->dir_size = 0;

    virtio_blk_write(SQFS_PART_START + inode_table_start / 512, buf, 512);

    /* ---- Step 3: Write directory table ---- */
    /*
     * Root directory entries:
     *   entry 1: inode_number=2, type=REG, name="hello.txt" (9 chars)
     *   entry 2: inode_number=3, type=REG, name="readme.txt" (10 chars)
     */
    mem_zero(buf, 512);
    {
        u32 pos = 0;

        /* Entry 1: hello.txt */
        de = (struct squashfs_dir_entry *)(buf + pos);
        de->inode_number = 2;
        de->inode_type = SQUASHFS_REG_TYPE;
        de->name_size = 9;
        mem_copy(buf + pos + sizeof(struct squashfs_dir_entry),
                 "hello.txt", 9);
        pos += sizeof(struct squashfs_dir_entry) + 9;  /* 8 + 9 = 17 */

        /* Entry 2: readme.txt */
        de = (struct squashfs_dir_entry *)(buf + pos);
        de->inode_number = 3;
        de->inode_type = SQUASHFS_REG_TYPE;
        de->name_size = 10;
        mem_copy(buf + pos + sizeof(struct squashfs_dir_entry),
                 "readme.txt", 10);
        pos += sizeof(struct squashfs_dir_entry) + 10; /* 8 + 10 = 18 */
    }

    virtio_blk_write(SQFS_PART_START + dir_table_start / 512, buf, 512);

    /* ---- Step 4: Write superblock ---- */
    mem_zero(buf, 512);
    dsb = (struct squashfs_super_block *)buf;
    dsb->s_magic = SQUASHFS_MAGIC;
    dsb->inodes = 3;
    dsb->mkfs_time = 0;
    dsb->block_size = SQUASHFS_DEFAULT_BLOCK_SIZE;
    dsb->fragments = 0;
    dsb->compression = SQUASHFS_COMP_NONE;
    dsb->block_log = SQUASHFS_DEFAULT_BLOCK_LOG;
    dsb->flags = 0;
    dsb->no_ids = 0;
    dsb->s_major = SQUASHFS_MAJOR;
    dsb->s_minor = SQUASHFS_MINOR;
    dsb->root_inode = 1;            /* root inode 编号 = 1 */
    dsb->bytes_used = dir_table_start + 512;
    dsb->inode_table_start = inode_table_start;
    dsb->directory_table_start = dir_table_start;

    virtio_blk_write(SQFS_PART_START, buf, 512);

    boot_printk("[squashfs] Test image built: 3 inodes, 2 files\n");
}
