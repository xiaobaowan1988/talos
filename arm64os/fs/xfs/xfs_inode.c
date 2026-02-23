/* SPDX-License-Identifier: GPL-2.0 */
/*
 * arm64os/fs/xfs/xfs_inode.c
 *
 * XFS inode 读写
 *
 * 参考：fs/xfs/xfs_inode.c
 *       fs/xfs/libxfs/xfs_inode_buf.c
 *
 * Phase 8 教学简化版：
 *   - inode 大小固定 256 字节
 *   - 使用简单的 inode 编号到磁盘位置的映射
 *   - 数据 fork 支持 local（shortform dir）和 extents
 */

#include <linux/types.h>
#include <linux/fs.h>
#include <linux/xfs_format.h>

/* 外部函数 */
void boot_printk(const char *s);
void boot_printk_hex(unsigned long val);
int virtio_blk_read(u64 sector, void *buf, u32 len);
int virtio_blk_write(u64 sector, const void *buf, u32 len);
struct inode *new_inode(struct super_block *sb);
int xfs_alloc_block(struct xfs_mount *mp, u32 agno, u32 *bno_out);
int xfs_log_write_record(struct xfs_mount *mp, u32 type,
                           const void *data, u32 data_len);

/* 操作集前向声明（定义在 xfs_dir2.c） */
extern const struct inode_operations xfs_dir_inode_ops;
extern const struct file_operations  xfs_dir_fops;
extern const struct inode_operations xfs_file_inode_ops;
extern const struct file_operations  xfs_file_fops;

/* XFS inode 内存私有数据池 */
#define MAX_XFS_INODE_INFO 32
static struct xfs_inode_info xfs_info_pool[MAX_XFS_INODE_INFO];
static int xfs_info_idx = 0;

/*
 * ============================================================
 * 内部辅助
 * ============================================================
 */
static void xfs_mem_copy(void *dst, const void *src, u32 len)
{
    u8 *d = (u8 *)dst;
    const u8 *s = (const u8 *)src;
    u32 i;
    for (i = 0; i < len; i++)
        d[i] = s[i];
}

static void xfs_mem_zero(void *dst, u32 len)
{
    u8 *p = (u8 *)dst;
    u32 i;
    for (i = 0; i < len; i++)
        p[i] = 0;
}

/*
 * ============================================================
 * xfs_ino_to_sector - 将 inode 号转换为磁盘扇区
 *
 * XFS inode 号编码：
 *   简化版：inode 号直接映射到块中的 inode 槽位。
 *   inode 128 = 根目录（第一个 inode 块从特定偏移开始）
 *
 * AG 0 布局（简化）：
 *   block 0: superblock
 *   block 1: AGF
 *   block 2: AGI
 *   block 3: bnobt root
 *   block 4: inobt root
 *   block 5-(5+LOG_BLOCKS-1): WAL log
 *   block (5+LOG_BLOCKS): first inode block
 *
 * inode 号 128 对应第一个 inode 块中的 slot 0。
 * ============================================================
 */
static u64 xfs_ino_to_disk_sector(struct xfs_mount *mp, u64 ino,
                                    u32 *slot_offset)
{
    u32 inode_block_start;
    u32 ino_relative;
    u32 block_idx;
    u32 slot_in_block;
    u64 sector;

    /*
     * 第一个 inode 块 = AG header blocks (5) + log blocks
     * inode 128 对应 slot 0 of first inode block
     */
    inode_block_start = 5 + mp->m_logblocks;

    /* ino 相对于 XFS_ROOT_INO 的偏移 */
    if (ino < XFS_ROOT_INO) {
        /* 无效 inode 号 */
        *slot_offset = 0;
        return 0;
    }

    ino_relative = (u32)(ino - XFS_ROOT_INO);
    block_idx = ino_relative / XFS_INODES_PER_BLOCK;
    slot_in_block = ino_relative % XFS_INODES_PER_BLOCK;

    *slot_offset = slot_in_block * XFS_INODE_SIZE;

    /* 磁盘扇区 = 分区起始 + (inode_block_start + block_idx) * 8 */
    sector = XFS_PART_START +
             (u64)(inode_block_start + block_idx) * (XFS_BLOCK_SIZE / 512);

    return sector;
}

/*
 * ============================================================
 * xfs_inode_read - 从磁盘读取 XFS inode
 *
 * @mp:  XFS 挂载信息
 * @ino: inode 号
 * @dip: 输出 — 磁盘格式 inode 结构体
 *
 * 返回 0 成功，-1 失败。
 *
 * 参考：fs/xfs/xfs_inode.c xfs_iread()
 * ============================================================
 */
int xfs_inode_read(struct xfs_mount *mp, u64 ino, struct xfs_dinode *dip)
{
    static u8 __attribute__((aligned(4096))) io_buf[4096];
    u32 slot_offset;
    u64 sector;

    sector = xfs_ino_to_disk_sector(mp, ino, &slot_offset);
    if (sector == 0)
        return -1;

    /* 读取整个 inode 块（4KB） */
    if (virtio_blk_read(sector, io_buf, XFS_BLOCK_SIZE) != 0) {
        boot_printk("[xfs] ERROR: read inode block failed\n");
        return -1;
    }

    xfs_mem_copy(dip, io_buf + slot_offset, sizeof(struct xfs_dinode));

    return 0;
}

/*
 * ============================================================
 * xfs_inode_write - 将 XFS inode 写回磁盘
 *
 * @mp:  XFS 挂载信息
 * @ino: inode 号
 * @dip: 磁盘格式 inode 结构体
 *
 * 返回 0 成功，-1 失败。
 *
 * 参考：fs/xfs/xfs_inode.c xfs_iflush()
 * ============================================================
 */
int xfs_inode_write(struct xfs_mount *mp, u64 ino, struct xfs_dinode *dip)
{
    static u8 __attribute__((aligned(4096))) io_buf[4096];
    u32 slot_offset;
    u64 sector;

    sector = xfs_ino_to_disk_sector(mp, ino, &slot_offset);
    if (sector == 0)
        return -1;

    /* 读取整个 inode 块 */
    if (virtio_blk_read(sector, io_buf, XFS_BLOCK_SIZE) != 0)
        return -1;

    /* 更新 inode 数据 */
    xfs_mem_copy(io_buf + slot_offset, dip, sizeof(struct xfs_dinode));

    /* 写回 */
    if (virtio_blk_write(sector, io_buf, XFS_BLOCK_SIZE) != 0)
        return -1;

    return 0;
}

/*
 * ============================================================
 * xfs_iget - 读取磁盘 inode 并构建 VFS inode
 *
 * @sb:  VFS 超级块
 * @ino: XFS inode 号
 *
 * 返回 VFS inode 指针，失败返回 NULL。
 *
 * 参考：fs/xfs/xfs_inode.c xfs_iget()
 * ============================================================
 */
struct inode *xfs_iget(struct super_block *sb, u64 ino)
{
    struct xfs_mount *mp = (struct xfs_mount *)sb->s_fs_info;
    struct xfs_dinode dip;
    struct inode *inode;
    struct xfs_inode_info *xi;
    u16 mode;

    if (xfs_inode_read(mp, ino, &dip) != 0)
        return NULL;

    /* 验证 inode 魔数 */
    if (be16_to_cpu(dip.di_magic) != XFS_DINODE_MAGIC) {
        boot_printk("[xfs] ERROR: bad inode magic at ino ");
        boot_printk_hex((unsigned long)ino);
        boot_printk("\n");
        return NULL;
    }

    /* 分配 VFS inode */
    inode = new_inode(sb);
    if (!inode)
        return NULL;

    /* 分配 XFS 私有数据 */
    if (xfs_info_idx >= MAX_XFS_INODE_INFO) {
        boot_printk("[xfs] ERROR: inode info pool exhausted\n");
        return NULL;
    }
    xi = &xfs_info_pool[xfs_info_idx++];

    /* 填充私有数据 */
    xi->ino = ino;
    xi->di_format = dip.di_format;
    xi->data_startblock = 0;
    xi->data_blockcount = 0;
    xi->dir_count = 0;

    /* 解析模式（大端序转换） */
    mode = be16_to_cpu(dip.di_mode);

    /* 设置 VFS inode 字段 */
    inode->i_ino = (unsigned long)ino;
    inode->i_mode = mode;
    inode->i_size = (unsigned long)be64_to_cpu(dip.di_size);
    inode->i_private = xi;

    if (S_ISDIR(mode)) {
        inode->i_op = &xfs_dir_inode_ops;
        inode->i_fop = &xfs_dir_fops;

        /* 缓存 shortform 目录数据 */
        if (dip.di_format == XFS_DINODE_FMT_LOCAL) {
            struct xfs_dir2_sf_hdr *sfh =
                (struct xfs_dir2_sf_hdr *)dip.di_datafork;
            xi->dir_count = be32_to_cpu(sfh->count);
            xfs_mem_copy(xi->dir_data, dip.di_datafork,
                          sizeof(xi->dir_data));
        }
    } else {
        inode->i_op = &xfs_file_inode_ops;
        inode->i_fop = &xfs_file_fops;

        /* 解析 extent 信息 */
        if (dip.di_format == XFS_DINODE_FMT_EXTENTS &&
            be64_to_cpu(dip.di_nblocks) > 0) {
            struct xfs_bmbt_rec *ext =
                (struct xfs_bmbt_rec *)dip.di_datafork;
            xi->data_startblock = (u32)be64_to_cpu(ext->br_startblock);
            xi->data_blockcount = be32_to_cpu(ext->br_blockcount);
        }
    }

    return inode;
}

/*
 * ============================================================
 * xfs_create_inode - 在磁盘上创建新 inode
 *
 * @mp:    XFS 挂载信息
 * @mode:  文件类型 + 权限
 * @ino_out: 输出分配的 inode 号
 *
 * 返回 0 成功，-1 失败。
 * ============================================================
 */
int xfs_create_inode(struct xfs_mount *mp, u16 mode, u64 *ino_out)
{
    struct xfs_dinode dip;
    u64 ino;

    /*
     * 简化 inode 分配：顺序递增。
     * 从 mp->m_icount + XFS_ROOT_INO 分配下一个 inode 号。
     */
    ino = XFS_ROOT_INO + mp->m_icount;
    mp->m_icount++;
    if (mp->m_ifree > 0)
        mp->m_ifree--;

    /* 构建磁盘 inode（大端序） */
    xfs_mem_zero(&dip, sizeof(dip));
    dip.di_magic = cpu_to_be16(XFS_DINODE_MAGIC);
    dip.di_mode = cpu_to_be16(mode);
    dip.di_nlink = cpu_to_be32(1);
    dip.di_size = cpu_to_be64(0);
    dip.di_nblocks = cpu_to_be64(0);
    dip.di_gen = cpu_to_be32(1);

    if (S_ISDIR(mode)) {
        dip.di_format = XFS_DINODE_FMT_LOCAL;
        /* 初始化空的 shortform 目录 */
        {
            struct xfs_dir2_sf_hdr *sfh =
                (struct xfs_dir2_sf_hdr *)dip.di_datafork;
            sfh->count = cpu_to_be32(0);
            sfh->parent = cpu_to_be64(0);
        }
    } else {
        dip.di_format = XFS_DINODE_FMT_EXTENTS;
    }

    /* 写入磁盘 */
    if (xfs_inode_write(mp, ino, &dip) != 0)
        return -1;

    /* 写日志 */
    xfs_log_write_record(mp, XFS_LOG_INODE_CREATE, &ino, sizeof(ino));

    *ino_out = ino;
    return 0;
}
