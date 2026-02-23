/* SPDX-License-Identifier: GPL-2.0 */
/*
 * arm64os/fs/squashfs/dir.c
 *
 * squashfs 目录读取
 *
 * 参考：fs/squashfs/dir.c
 *       fs/squashfs/namei.c
 *
 * Phase 8 教学简化版：
 *   - 从磁盘 directory table 读取目录项
 *   - lookup 通过线性扫描目录项匹配文件名
 *   - 目录项格式：8字节头 + 变长文件名
 */

#include <linux/types.h>
#include <linux/fs.h>
#include <linux/squashfs_fs.h>

/* 外部函数 */
void boot_printk(const char *s);
void boot_printk_hex(unsigned long val);
int virtio_blk_read(u64 sector, void *buf, u32 len);
struct inode *squashfs_iget(struct super_block *sb, u32 inode_number);
struct dentry *dget(struct dentry *dentry);
void d_add(struct dentry *dentry, struct inode *inode);

/*
 * ============================================================
 * 内部辅助函数
 * ============================================================
 */
static int name_equal(const char *a, unsigned int alen,
                       const char *b, unsigned int blen)
{
    unsigned int i;

    if (alen != blen)
        return 0;

    for (i = 0; i < alen; i++) {
        if (a[i] != b[i])
            return 0;
    }
    return 1;
}

/*
 * ============================================================
 * squashfs_lookup - 在 squashfs 目录中查找文件
 *
 * VFS namei.c 的 walk_component() 在 dcache miss 后调用此函数。
 * 从磁盘 directory table 读取目录项，匹配文件名。
 *
 * @dir:    目录 inode
 * @dentry: 待查找的 dentry（name 已设置）
 * @flags:  查找标志（未使用）
 *
 * 返回已关联 inode 的 dentry，未找到返回传入的负 dentry。
 *
 * 参考：fs/squashfs/namei.c squashfs_lookup()
 * ============================================================
 */
struct dentry *squashfs_lookup(struct inode *dir, struct dentry *dentry,
                                unsigned int flags)
{
    struct squashfs_inode_info *si;
    u64 dir_disk_offset;
    u64 sector;
    u32 sector_offset;
    u32 remaining;
    u32 pos;
    const char *search_name;
    unsigned int search_len;
    /* 读取缓冲区 — 足够容纳目录表（目录不会太大） */
    static u8 __attribute__((aligned(4096))) dir_buf[4096];

    (void)flags;

    if (!dir || !dir->i_private)
        return dentry;  /* 返回负 dentry */

    si = (struct squashfs_inode_info *)dir->i_private;
    search_name = dentry->d_name.name;
    search_len = dentry->d_name.len;

    /* 计算 directory table 在磁盘上的位置 */
    dir_disk_offset = si->dir_table_start + si->dir_offset;
    sector = SQFS_PART_START + dir_disk_offset / 512;
    sector_offset = (u32)(dir_disk_offset % 512);

    /* 读取包含目录数据的扇区（最多 8 扇区 = 4KB） */
    {
        u32 read_sectors = (si->dir_size + sector_offset + 511) / 512;
        if (read_sectors > 8)
            read_sectors = 8;
        if (virtio_blk_read(sector, dir_buf, read_sectors * 512) != 0) {
            boot_printk("[squashfs] ERROR: read dir table failed\n");
            return dentry;
        }
    }

    /* 扫描目录项 */
    remaining = si->dir_size;
    pos = sector_offset;

    while (remaining >= sizeof(struct squashfs_dir_entry)) {
        struct squashfs_dir_entry *de;
        u32 entry_size;
        const char *entry_name;

        de = (struct squashfs_dir_entry *)(dir_buf + pos);
        entry_name = (const char *)(dir_buf + pos + sizeof(struct squashfs_dir_entry));
        entry_size = sizeof(struct squashfs_dir_entry) + de->name_size;

        if (entry_size > remaining)
            break;

        /* 匹配文件名 */
        if (name_equal(search_name, search_len,
                        entry_name, de->name_size)) {
            /* 找到：加载对应 inode */
            struct inode *inode;

            inode = squashfs_iget(dir->i_sb, de->inode_number);
            if (inode) {
                d_add(dentry, inode);
                return dentry;
            }
        }

        pos += entry_size;
        remaining -= entry_size;
    }

    /* 未找到：返回负 dentry */
    return dentry;
}

/*
 * ============================================================
 * 操作集定义
 * ============================================================
 */

/* squashfs 是只读文件系统，不支持 create/mkdir/unlink */
const struct inode_operations squashfs_dir_inode_ops = {
    .lookup = squashfs_lookup,
    .create = NULL,
    .mkdir  = NULL,
    .unlink = NULL,
};

/* 目录文件操作集（不支持直接读写目录） */
const struct file_operations squashfs_dir_fops = {
    .read    = NULL,
    .write   = NULL,
    .open    = NULL,
    .release = NULL,
};
