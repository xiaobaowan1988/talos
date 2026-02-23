/* SPDX-License-Identifier: GPL-2.0 */
/*
 * arm64os/fs/xfs/xfs_dir2.c
 *
 * XFS 目录 B+树（shortform 简化版）
 *
 * 参考：fs/xfs/libxfs/xfs_dir2.c
 *       fs/xfs/libxfs/xfs_dir2_sf.c
 *
 * Phase 8 教学简化版：
 *   - 只支持 shortform 目录（条目内嵌在 inode 数据区）
 *   - 最大约 8 个条目（受 176 字节数据 fork 限制）
 *   - 支持 lookup 和 create 操作
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
struct inode *xfs_iget(struct super_block *sb, u64 ino);
int xfs_inode_read(struct xfs_mount *mp, u64 ino, struct xfs_dinode *dip);
int xfs_inode_write(struct xfs_mount *mp, u64 ino, struct xfs_dinode *dip);
int xfs_create_inode(struct xfs_mount *mp, u16 mode, u64 *ino_out);
int xfs_alloc_block(struct xfs_mount *mp, u32 agno, u32 *bno_out);
int xfs_log_write_record(struct xfs_mount *mp, u32 type,
                           const void *data, u32 data_len);
int xfs_log_commit(struct xfs_mount *mp);
void d_add(struct dentry *dentry, struct inode *inode);
struct dentry *dget(struct dentry *dentry);

/*
 * ============================================================
 * 内部辅助
 * ============================================================
 */
static int xfs_name_equal(const char *a, u32 alen,
                            const char *b, u32 blen)
{
    u32 i;
    if (alen != blen)
        return 0;
    for (i = 0; i < alen; i++) {
        if (a[i] != b[i])
            return 0;
    }
    return 1;
}

static void xfs_dir_mem_copy(void *dst, const void *src, u32 len)
{
    u8 *d = (u8 *)dst;
    const u8 *s = (const u8 *)src;
    u32 i;
    for (i = 0; i < len; i++)
        d[i] = s[i];
}

static void xfs_dir_mem_zero(void *dst, u32 len)
{
    u8 *p = (u8 *)dst;
    u32 i;
    for (i = 0; i < len; i++)
        p[i] = 0;
}

/*
 * ============================================================
 * xfs_dir_lookup - 在 XFS 目录中查找文件
 *
 * 扫描 shortform 目录的条目，匹配文件名。
 *
 * @dir:    目录 inode
 * @dentry: 待查找的 dentry
 * @flags:  查找标志
 *
 * 返回已关联 inode 的 dentry。
 *
 * 参考：fs/xfs/xfs_iops.c xfs_vn_lookup()
 * ============================================================
 */
struct dentry *xfs_dir_lookup(struct inode *dir, struct dentry *dentry,
                                unsigned int flags)
{
    struct xfs_inode_info *xi;
    struct xfs_dir2_sf_hdr *sfh;
    u8 *data;
    u32 count;
    u32 pos;
    u32 i;
    const char *search_name;
    u32 search_len;

    (void)flags;

    if (!dir || !dir->i_private)
        return dentry;

    xi = (struct xfs_inode_info *)dir->i_private;
    search_name = dentry->d_name.name;
    search_len = dentry->d_name.len;

    /* 解析 shortform 目录 */
    sfh = (struct xfs_dir2_sf_hdr *)xi->dir_data;
    count = be32_to_cpu(sfh->count);
    data = xi->dir_data + sizeof(struct xfs_dir2_sf_hdr);

    for (i = 0; i < count; i++) {
        struct xfs_dir2_sf_entry *ent = (struct xfs_dir2_sf_entry *)data;
        u64 entry_ino = be64_to_cpu(ent->inumber);
        u8 namelen = ent->namelen;
        const char *name = (const char *)(data + sizeof(struct xfs_dir2_sf_entry));

        if (xfs_name_equal(search_name, search_len, name, namelen)) {
            /* 找到：加载 inode */
            struct inode *inode = xfs_iget(dir->i_sb, entry_ino);
            if (inode) {
                d_add(dentry, inode);
                return dentry;
            }
        }

        /* 跳到下一个条目 */
        pos = sizeof(struct xfs_dir2_sf_entry) + namelen;
        data += pos;
    }

    /* 未找到：返回负 dentry */
    return dentry;
}

/*
 * ============================================================
 * xfs_dir_add_entry - 向 shortform 目录添加条目
 *
 * @mp:        XFS 挂载信息
 * @dir_ino:   目录 inode 号
 * @child_ino: 子条目 inode 号
 * @name:      文件名
 * @namelen:   文件名长度
 * @ftype:     文件类型
 *
 * 返回 0 成功，-1 失败。
 *
 * 参考：fs/xfs/libxfs/xfs_dir2_sf.c xfs_dir2_sf_addname()
 * ============================================================
 */
int xfs_dir_add_entry(struct xfs_mount *mp, u64 dir_ino,
                        u64 child_ino, const char *name, u8 namelen,
                        u8 ftype)
{
    struct xfs_dinode dip;
    struct xfs_dir2_sf_hdr *sfh;
    u8 *data;
    u32 count;
    u32 pos;
    u32 i;
    struct xfs_dir2_sf_entry *new_ent;
    u32 entry_size;

    (void)ftype;

    /* 读取目录 inode */
    if (xfs_inode_read(mp, dir_ino, &dip) != 0)
        return -1;

    if (dip.di_format != XFS_DINODE_FMT_LOCAL) {
        boot_printk("[xfs] ERROR: dir not shortform\n");
        return -1;
    }

    /* 解析当前 shortform 数据 */
    sfh = (struct xfs_dir2_sf_hdr *)dip.di_datafork;
    count = be32_to_cpu(sfh->count);

    /* 找到条目数据的末尾位置 */
    data = dip.di_datafork + sizeof(struct xfs_dir2_sf_hdr);
    for (i = 0; i < count; i++) {
        struct xfs_dir2_sf_entry *ent = (struct xfs_dir2_sf_entry *)data;
        data += sizeof(struct xfs_dir2_sf_entry) + ent->namelen;
    }

    /* 检查空间 */
    entry_size = sizeof(struct xfs_dir2_sf_entry) + namelen;
    pos = (u32)(data - dip.di_datafork);
    if (pos + entry_size > sizeof(dip.di_datafork)) {
        boot_printk("[xfs] ERROR: dir data fork full\n");
        return -1;
    }

    /* 添加新条目 */
    new_ent = (struct xfs_dir2_sf_entry *)data;
    new_ent->inumber = cpu_to_be64(child_ino);
    new_ent->namelen = namelen;
    new_ent->ftype = 0;
    xfs_dir_mem_copy(data + sizeof(struct xfs_dir2_sf_entry),
                      name, namelen);

    /* 更新计数 */
    sfh->count = cpu_to_be32(count + 1);

    /* 更新 inode 大小 */
    dip.di_size = cpu_to_be64((u64)(pos + entry_size));

    /* 写回磁盘 */
    if (xfs_inode_write(mp, dir_ino, &dip) != 0)
        return -1;

    /* 写日志 */
    xfs_log_write_record(mp, XFS_LOG_DIR_ADD, &child_ino, sizeof(child_ino));

    return 0;
}

/*
 * ============================================================
 * xfs_dir_create - VFS create 回调
 *
 * 在 XFS 目录中创建新文件。
 *
 * @dir:    父目录 inode
 * @dentry: 新文件的 dentry
 * @mode:   文件权限
 * @excl:   排他创建
 *
 * 返回 0 成功。
 *
 * 参考：fs/xfs/xfs_iops.c xfs_vn_create()
 * ============================================================
 */
int xfs_dir_create(struct inode *dir, struct dentry *dentry,
                     unsigned int mode, bool excl)
{
    struct xfs_mount *mp;
    struct xfs_inode_info *dir_xi;
    struct inode *inode;
    u64 new_ino;
    u32 data_bno;

    (void)excl;

    if (!dir || !dir->i_sb || !dir->i_sb->s_fs_info)
        return -1;

    mp = (struct xfs_mount *)dir->i_sb->s_fs_info;
    dir_xi = (struct xfs_inode_info *)dir->i_private;

    /* 创建新 inode */
    if (xfs_create_inode(mp, (u16)mode, &new_ino) != 0) {
        boot_printk("[xfs] ERROR: create inode failed\n");
        return -1;
    }

    /* 为普通文件分配一个数据块 */
    if (S_ISREG(mode)) {
        struct xfs_dinode dip;

        if (xfs_alloc_block(mp, 0, &data_bno) != 0) {
            boot_printk("[xfs] ERROR: alloc data block failed\n");
            return -1;
        }

        /* 更新 inode 的 extent 信息 */
        if (xfs_inode_read(mp, new_ino, &dip) == 0) {
            struct xfs_bmbt_rec *ext =
                (struct xfs_bmbt_rec *)dip.di_datafork;
            dip.di_format = XFS_DINODE_FMT_EXTENTS;
            dip.di_nblocks = cpu_to_be64(1);
            ext->br_startoff = cpu_to_be64(0);
            ext->br_startblock = cpu_to_be64((u64)data_bno);
            ext->br_blockcount = cpu_to_be32(1);
            xfs_inode_write(mp, new_ino, &dip);
        }
    }

    /* 向父目录添加条目 */
    if (xfs_dir_add_entry(mp, dir_xi->ino,
                            new_ino, dentry->d_name.name,
                            (u8)dentry->d_name.len, 0) != 0) {
        return -1;
    }

    /* 更新父目录的内存缓存 */
    {
        struct xfs_dinode dir_dip;
        if (xfs_inode_read(mp, dir_xi->ino, &dir_dip) == 0) {
            xfs_dir_mem_copy(dir_xi->dir_data, dir_dip.di_datafork,
                              sizeof(dir_xi->dir_data));
            {
                struct xfs_dir2_sf_hdr *sfh =
                    (struct xfs_dir2_sf_hdr *)dir_xi->dir_data;
                dir_xi->dir_count = be32_to_cpu(sfh->count);
            }
        }
    }

    /* 提交事务 */
    xfs_log_commit(mp);

    /* 构建 VFS inode 并关联到 dentry */
    inode = xfs_iget(dir->i_sb, new_ino);
    if (!inode)
        return -1;

    d_add(dentry, inode);

    return 0;
}

/*
 * ============================================================
 * xfs_file_read - 读取 XFS 文件数据
 *
 * @filp:  file 对象
 * @buf:   目标缓冲区
 * @count: 请求读取的字节数
 * @pos:   读写偏移指针
 *
 * 返回实际读取的字节数。
 *
 * 参考：fs/xfs/xfs_file.c xfs_file_read_iter()
 * ============================================================
 */
static ssize_t xfs_file_read(struct file *filp, char *buf, size_t count,
                               unsigned long *pos)
{
    struct inode *inode = filp->f_inode;
    struct xfs_inode_info *xi;
    u64 disk_sector;
    u32 block_offset;
    u32 avail;
    static u8 __attribute__((aligned(4096))) data_buf[4096];

    if (!inode || !inode->i_private || !inode->i_sb || !inode->i_sb->s_fs_info)
        return 0;

    xi = (struct xfs_inode_info *)inode->i_private;

    /* 检查偏移 */
    if (*pos >= inode->i_size)
        return 0;

    avail = (u32)(inode->i_size - *pos);
    if (count > avail)
        count = avail;

    if (count == 0)
        return 0;

    /* 计算磁盘位置 */
    block_offset = (u32)(*pos % XFS_BLOCK_SIZE);
    disk_sector = XFS_PART_START +
                  (u64)xi->data_startblock * (XFS_BLOCK_SIZE / 512);

    /* 读取数据块 */
    if (virtio_blk_read(disk_sector, data_buf, XFS_BLOCK_SIZE) != 0) {
        boot_printk("[xfs] ERROR: read data block failed\n");
        return 0;
    }

    /* 限制读取量不超过单块 */
    if (count > XFS_BLOCK_SIZE - block_offset)
        count = XFS_BLOCK_SIZE - block_offset;

    xfs_dir_mem_copy(buf, (char *)data_buf + block_offset, (u32)count);

    *pos += count;
    return (ssize_t)count;
}

/*
 * ============================================================
 * xfs_file_write - 写入 XFS 文件数据
 *
 * @filp:  file 对象
 * @buf:   源数据缓冲区
 * @count: 写入字节数
 * @pos:   读写偏移指针
 *
 * 返回实际写入的字节数。
 *
 * 参考：fs/xfs/xfs_file.c xfs_file_write_iter()
 * ============================================================
 */
static ssize_t xfs_file_write(struct file *filp, const char *buf,
                                size_t count, unsigned long *pos)
{
    struct inode *inode = filp->f_inode;
    struct xfs_inode_info *xi;
    struct xfs_mount *mp;
    u64 disk_sector;
    u32 block_offset;
    u32 end;
    static u8 __attribute__((aligned(4096))) data_buf[4096];

    if (!inode || !inode->i_private || !inode->i_sb || !inode->i_sb->s_fs_info)
        return -1;

    xi = (struct xfs_inode_info *)inode->i_private;
    mp = (struct xfs_mount *)inode->i_sb->s_fs_info;

    /* 检查是否有数据块 */
    if (xi->data_blockcount == 0) {
        boot_printk("[xfs] ERROR: file has no data block\n");
        return -1;
    }

    block_offset = (u32)(*pos % XFS_BLOCK_SIZE);

    /* 限制写入量不超过单块 */
    end = block_offset + (u32)count;
    if (end > XFS_BLOCK_SIZE)
        count = XFS_BLOCK_SIZE - block_offset;

    /* 读取现有数据块（read-modify-write） */
    disk_sector = XFS_PART_START +
                  (u64)xi->data_startblock * (XFS_BLOCK_SIZE / 512);

    if (virtio_blk_read(disk_sector, data_buf, XFS_BLOCK_SIZE) != 0) {
        /* 新块可能还没数据，清零 */
        xfs_dir_mem_zero(data_buf, XFS_BLOCK_SIZE);
    }

    /* 写入数据 */
    xfs_dir_mem_copy((char *)data_buf + block_offset, buf, (u32)count);

    /* 写回磁盘 */
    if (virtio_blk_write(disk_sector, data_buf, XFS_BLOCK_SIZE) != 0) {
        boot_printk("[xfs] ERROR: write data block failed\n");
        return -1;
    }

    *pos += count;

    /* 更新文件大小 */
    if (*pos > inode->i_size) {
        struct xfs_dinode dip;

        inode->i_size = *pos;

        /* 更新磁盘 inode 的大小 */
        if (xfs_inode_read(mp, xi->ino, &dip) == 0) {
            dip.di_size = cpu_to_be64(*pos);
            xfs_inode_write(mp, xi->ino, &dip);
        }
    }

    /* 写日志 */
    xfs_log_write_record(mp, XFS_LOG_INODE_UPDATE, &xi->ino, sizeof(xi->ino));
    xfs_log_commit(mp);

    return (ssize_t)count;
}

/*
 * ============================================================
 * 操作集定义
 * ============================================================
 */

const struct inode_operations xfs_dir_inode_ops = {
    .lookup = xfs_dir_lookup,
    .create = xfs_dir_create,
    .mkdir  = NULL,     /* Phase 8 不支持 mkdir on XFS */
    .unlink = NULL,
};

const struct file_operations xfs_dir_fops = {
    .read    = NULL,
    .write   = NULL,
    .open    = NULL,
    .release = NULL,
};

const struct inode_operations xfs_file_inode_ops = {
    .lookup = NULL,
    .create = NULL,
    .mkdir  = NULL,
    .unlink = NULL,
};

const struct file_operations xfs_file_fops = {
    .read    = xfs_file_read,
    .write   = xfs_file_write,
    .open    = NULL,
    .release = NULL,
};
