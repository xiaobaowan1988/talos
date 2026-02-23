/* SPDX-License-Identifier: GPL-2.0 */
/*
 * arm64os/fs/squashfs/inode.c
 *
 * squashfs inode 解析
 *
 * 参考：fs/squashfs/inode.c
 *
 * Phase 8 教学简化版：
 *   - 从磁盘 inode table 读取固定大小 inode（48字节）
 *   - 构建 VFS inode 并关联 squashfs 私有数据
 *   - inode 编号从 1 开始（inode 1 = 根目录）
 */

#include <linux/types.h>
#include <linux/fs.h>
#include <linux/squashfs_fs.h>

/* 外部函数 */
void boot_printk(const char *s);
void boot_printk_hex(unsigned long val);
int virtio_blk_read(u64 sector, void *buf, u32 len);
struct inode *new_inode(struct super_block *sb);

/* 操作集（定义在 dir.c 和 file.c） */
extern const struct inode_operations squashfs_dir_inode_ops;
extern const struct file_operations  squashfs_dir_fops;
extern const struct inode_operations squashfs_file_inode_ops;
extern const struct file_operations  squashfs_file_fops;

/* squashfs_inode_info 静态池 */
#define MAX_SQFS_INODE_INFO  32
static struct squashfs_inode_info sqfs_info_pool[MAX_SQFS_INODE_INFO];
static int sqfs_info_idx = 0;

/*
 * ============================================================
 * squashfs_read_inode - 从磁盘读取 squashfs inode
 *
 * @sb:           VFS 超级块
 * @raw:          输出 — 磁盘格式 inode 结构体
 * @inode_number: inode 编号（从 1 开始）
 *
 * 返回 0 成功，-1 失败。
 *
 * 参考：fs/squashfs/inode.c squashfs_read_inode()
 * ============================================================
 */
int squashfs_read_inode(struct super_block *sb, struct squashfs_inode *raw,
                         u32 inode_number)
{
    struct squashfs_sb_info *sbi = (struct squashfs_sb_info *)sb->s_fs_info;
    u64 inode_offset;
    u64 disk_byte_offset;
    u64 sector;
    u32 sector_offset;
    static u8 __attribute__((aligned(4096))) io_buf[512];

    if (!sbi || inode_number == 0)
        return -1;

    /*
     * inode table 中的偏移：
     * inode_number 从 1 开始，inode 0 不存在
     * 每个 inode 固定 48 字节
     */
    inode_offset = (u64)(inode_number - 1) * sizeof(struct squashfs_inode);
    disk_byte_offset = sbi->inode_table_start + inode_offset;

    /*
     * 转换为扇区地址：
     * 分区起始扇区 + 字节偏移 / 512
     */
    sector = SQFS_PART_START + disk_byte_offset / 512;
    sector_offset = (u32)(disk_byte_offset % 512);

    /* 读取包含该 inode 的扇区 */
    if (virtio_blk_read(sector, io_buf, 512) != 0) {
        boot_printk("[squashfs] ERROR: read inode failed\n");
        return -1;
    }

    /* 复制 inode 数据 */
    {
        u8 *src = io_buf + sector_offset;
        u8 *dst = (u8 *)raw;
        u32 i;

        /* 确保不跨扇区边界（48 字节 inode 在 512 扇区内） */
        for (i = 0; i < sizeof(struct squashfs_inode); i++)
            dst[i] = src[i];
    }

    return 0;
}

/*
 * ============================================================
 * squashfs_iget - 读取磁盘 inode 并构建 VFS inode
 *
 * @sb:           VFS 超级块
 * @inode_number: squashfs inode 编号
 *
 * 返回 VFS inode 指针，失败返回 NULL。
 *
 * 参考：fs/squashfs/inode.c squashfs_iget()
 * ============================================================
 */
struct inode *squashfs_iget(struct super_block *sb, u32 inode_number)
{
    struct squashfs_sb_info *sbi = (struct squashfs_sb_info *)sb->s_fs_info;
    struct squashfs_inode raw;
    struct inode *inode;
    struct squashfs_inode_info *si;

    /* 从磁盘读取 squashfs inode */
    if (squashfs_read_inode(sb, &raw, inode_number) != 0)
        return NULL;

    /* 分配 VFS inode */
    inode = new_inode(sb);
    if (!inode)
        return NULL;

    /* 分配 squashfs 私有数据 */
    if (sqfs_info_idx >= MAX_SQFS_INODE_INFO) {
        boot_printk("[squashfs] ERROR: inode info pool exhausted\n");
        return NULL;
    }
    si = &sqfs_info_pool[sqfs_info_idx++];

    /* 填充私有数据 */
    si->block_size = sbi->block_size;
    si->start_block = raw.start_block;
    si->block_count = raw.block_count;
    si->dir_offset = raw.dir_offset;
    si->dir_size = raw.dir_size;
    si->compression = sbi->compression;
    si->inode_table_start = sbi->inode_table_start;
    si->dir_table_start = sbi->dir_table_start;

    /* 填充 VFS inode */
    inode->i_ino = inode_number;
    inode->i_size = raw.file_size;
    inode->i_private = si;

    if (raw.inode_type == SQUASHFS_DIR_TYPE) {
        inode->i_mode = S_IFDIR | raw.mode;
        inode->i_op = &squashfs_dir_inode_ops;
        inode->i_fop = &squashfs_dir_fops;
    } else {
        inode->i_mode = S_IFREG | raw.mode;
        inode->i_op = &squashfs_file_inode_ops;
        inode->i_fop = &squashfs_file_fops;
    }

    return inode;
}
