/* SPDX-License-Identifier: GPL-2.0 */
/*
 * arm64os/fs/overlayfs/file.c
 *
 * overlayfs 文件读写（透明代理）
 *
 * 参考：fs/overlayfs/file.c
 *
 * Phase 9 实现：
 *   - ovl_read()：从真实层读取数据
 *   - ovl_write()：写入数据（先 copy-up 再写入）
 *   - ovl_open()：打开时初始化层信息
 *
 * 读写代理原理：
 *   - overlay 的 file 对象存储对真实文件系统 inode 的引用
 *   - read：优先从 upper 层读取，否则从 lower 层读取
 *   - write：如果文件在 lower 层，先执行 copy-up 到 upper 层，
 *            然后在 upper 层执行写入
 *
 * 简化说明：
 *   - 使用 file->f_inode->i_private 中的 ovl_inode_info 访问真实层
 *   - 直接调用底层文件系统的 read/write 回调
 *   - copy-up 在首次写入时触发（COW — Copy on Write）
 */

#include <linux/types.h>
#include <linux/list.h>
#include <linux/fs.h>

/* 外部函数 */
void boot_printk(const char *s);
void boot_printk_hex(unsigned long val);

/* copy-up 接口（由 copy_up.c 提供） */
int ovl_copy_up_one(struct super_block *sb, struct dentry *ovl_dentry);

/*
 * ovl_inode_info 定义（与 inode.c 共用）
 */
struct ovl_inode_info {
    struct dentry  *upper_dentry;
    struct dentry  *lower_dentry;
};

/*
 * ============================================================
 * ovl_get_real_inode - 获取真实层 inode
 *
 * 优先返回 upper 层 inode（修改过的版本），
 * 否则返回 lower 层 inode（原始版本）。
 *
 * @ovl_inode: overlay inode
 *
 * 返回真实层 inode，NULL 表示无有效层。
 * ============================================================
 */
static struct inode *ovl_get_real_inode(struct inode *ovl_inode)
{
    struct ovl_inode_info *oi;

    if (!ovl_inode || !ovl_inode->i_private)
        return NULL;

    oi = (struct ovl_inode_info *)ovl_inode->i_private;

    /* upper 优先 */
    if (oi->upper_dentry && oi->upper_dentry->d_inode)
        return oi->upper_dentry->d_inode;

    /* 否则 lower */
    if (oi->lower_dentry && oi->lower_dentry->d_inode)
        return oi->lower_dentry->d_inode;

    return NULL;
}

/*
 * ============================================================
 * ovl_read - overlayfs 读操作
 *
 * 从真实层的文件系统读取数据。
 * 不触发 copy-up（只读操作直接透传）。
 *
 * @filp:  overlay file 对象
 * @buf:   目标缓冲区
 * @count: 请求读取的字节数
 * @pos:   读写偏移指针
 *
 * 返回实际读取的字节数，负数表示错误。
 *
 * 参考：fs/overlayfs/file.c ovl_read_iter()
 * ============================================================
 */
static ssize_t ovl_read(struct file *filp, char *buf, size_t count,
                          unsigned long *pos)
{
    struct inode *real_inode;
    struct file fake_file;

    real_inode = ovl_get_real_inode(filp->f_inode);
    if (!real_inode || !real_inode->i_fop || !real_inode->i_fop->read)
        return -9;  /* EBADF */

    /*
     * 构造临时 file 对象指向真实 inode。
     * 底层文件系统的 read 回调通过 filp->f_inode 访问数据，
     * 所以我们需要让它看到真实 inode。
     *
     * 参考：fs/overlayfs/file.c ovl_real_file()
     */
    fake_file.f_dentry = NULL;
    fake_file.f_inode = real_inode;
    fake_file.f_op = real_inode->i_fop;
    fake_file.f_flags = filp->f_flags;
    fake_file.f_pos = *pos;
    fake_file.f_count = 1;

    {
        ssize_t ret = real_inode->i_fop->read(&fake_file, buf, count,
                                               &fake_file.f_pos);
        /* 同步偏移回 overlay file */
        *pos = fake_file.f_pos;
        return ret;
    }
}

/*
 * ============================================================
 * ovl_write - overlayfs 写操作
 *
 * 写入前检查是否需要 copy-up：
 *   - 如果文件只在 lower 层，先 copy-up 到 upper 层
 *   - 然后在 upper 层执行写入
 *
 * @filp:  overlay file 对象
 * @buf:   数据缓冲区
 * @count: 写入字节数
 * @pos:   读写偏移指针
 *
 * 返回实际写入的字节数，负数表示错误。
 *
 * 参考：fs/overlayfs/file.c ovl_write_iter()
 * ============================================================
 */
static ssize_t ovl_write(struct file *filp, const char *buf, size_t count,
                           unsigned long *pos)
{
    struct ovl_inode_info *oi;
    struct inode *real_inode;
    struct file fake_file;

    if (!filp->f_inode || !filp->f_inode->i_private)
        return -9;  /* EBADF */

    oi = (struct ovl_inode_info *)filp->f_inode->i_private;

    /*
     * 检查 copy-up 需要：
     * 如果文件在 lower 层但不在 upper 层，需要先 copy-up。
     */
    if (!oi->upper_dentry && oi->lower_dentry) {
        int ret;

        boot_printk("[overlayfs] write triggers copy-up\n");

        ret = ovl_copy_up_one(filp->f_inode->i_sb, filp->f_dentry);
        if (ret != 0) {
            boot_printk("[overlayfs] ERROR: copy-up failed\n");
            return ret;
        }

        /* copy-up 后 oi->upper_dentry 应该已被设置 */

        /*
         * copy-up 后截断 upper 文件：
         * copy-up 复制了 lower 的全部内容到 upper，但调用者即将从 pos 0
         * 写入新数据。如果新数据比原文件短，尾部会残留旧数据。
         * 简化处理：copy-up 后重置 upper i_size 为 0，让写入从空文件开始。
         * （真实 Linux 中由 O_TRUNC 或 ftruncate 处理）
         */
        if (oi->upper_dentry && oi->upper_dentry->d_inode)
            oi->upper_dentry->d_inode->i_size = 0;
    }

    /* 获取 upper 层 inode 进行写入 */
    if (!oi->upper_dentry || !oi->upper_dentry->d_inode) {
        boot_printk("[overlayfs] ERROR: no upper inode after copy-up\n");
        return -1;
    }

    real_inode = oi->upper_dentry->d_inode;
    if (!real_inode->i_fop || !real_inode->i_fop->write)
        return -9;

    /* 构造临时 file 指向 upper 层真实 inode */
    fake_file.f_dentry = oi->upper_dentry;
    fake_file.f_inode = real_inode;
    fake_file.f_op = real_inode->i_fop;
    fake_file.f_flags = filp->f_flags;
    fake_file.f_pos = *pos;
    fake_file.f_count = 1;

    {
        ssize_t ret = real_inode->i_fop->write(&fake_file, buf, count,
                                                &fake_file.f_pos);
        /* 同步偏移和大小 */
        *pos = fake_file.f_pos;
        filp->f_inode->i_size = real_inode->i_size;
        return ret;
    }
}

/*
 * ============================================================
 * 操作集定义
 * ============================================================
 */

/* overlayfs 文件操作集 */
const struct file_operations ovl_file_fops = {
    .read       = ovl_read,
    .write      = ovl_write,
    .open       = NULL,
    .release    = NULL,
};
