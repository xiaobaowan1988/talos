/* SPDX-License-Identifier: GPL-2.0 */
/*
 * arm64os/fs/overlayfs/copy_up.c
 *
 * overlayfs copy-up 机制核心
 *
 * 参考：fs/overlayfs/copy_up.c
 *
 * Phase 9 实现：
 *   - ovl_copy_up_one()：将文件从 lower 层复制到 upper 层
 *
 * copy-up 原理（写时复制 COW）：
 *   当用户写入一个只存在于 lower 层（只读）的文件时：
 *   1. 在 work 目录创建临时文件（原子性保证）
 *   2. 从 lower 层读取文件全部内容
 *   3. 将内容写入临时文件
 *   4. 复制文件元数据（权限、大小等）
 *   5. 原子 rename：work/tmp → upper/filename
 *   6. 更新 overlay inode，后续操作走 upper 层
 *
 * 简化说明：
 *   - 跳过 work 目录原子 rename，直接在 upper 创建
 *     （我们的内核不支持 rename，且单 CPU 无并发问题）
 *   - 不复制 xattr、安全标签等
 *   - 文件最大 4KB（受 ramfs 单页限制）
 */

#include <linux/types.h>
#include <linux/list.h>
#include <linux/fs.h>

/* 外部函数 */
void boot_printk(const char *s);
void boot_printk_hex(unsigned long val);

/* VFS 接口 */
struct dentry *d_alloc(struct dentry *parent, const struct qstr *name);
struct dentry *d_lookup(const struct dentry *parent, const struct qstr *name);
void d_add(struct dentry *dentry, struct inode *inode);
unsigned int full_name_hash(const void *salt, const char *name, unsigned int len);
struct inode *new_inode(struct super_block *sb);

/*
 * 与其他 overlayfs 源文件共用的数据结构定义
 */
struct ovl_fs {
    struct dentry  *lower_root;
    struct dentry  *upper_root;
    struct dentry  *work_root;
};

struct ovl_inode_info {
    struct dentry  *upper_dentry;
    struct dentry  *lower_dentry;
};

/* copy-up 临时缓冲区（4KB，页对齐） */
static char __attribute__((aligned(4096))) copyup_buf[4096];

/*
 * ============================================================
 * ovl_copy_data - 从 lower inode 读取数据到缓冲区
 *
 * @lower_inode: lower 层文件 inode
 * @buf:         目标缓冲区
 * @size:        要复制的字节数
 *
 * 返回实际读取的字节数，负数表示错误。
 * ============================================================
 */
static ssize_t ovl_copy_data(struct inode *lower_inode, char *buf,
                               unsigned long size)
{
    struct file fake_file;
    unsigned long pos = 0;

    if (!lower_inode->i_fop || !lower_inode->i_fop->read)
        return -1;

    /* 构造临时 file 用于底层读取 */
    fake_file.f_dentry = NULL;
    fake_file.f_inode = lower_inode;
    fake_file.f_op = lower_inode->i_fop;
    fake_file.f_flags = 0;     /* O_RDONLY */
    fake_file.f_pos = 0;
    fake_file.f_count = 1;

    return lower_inode->i_fop->read(&fake_file, buf, size, &pos);
}

/*
 * ============================================================
 * ovl_write_data - 将数据写入 upper inode
 *
 * @upper_inode: upper 层文件 inode
 * @buf:         源数据
 * @size:        写入字节数
 *
 * 返回实际写入的字节数，负数表示错误。
 * ============================================================
 */
static ssize_t ovl_write_data(struct inode *upper_inode,
                                struct dentry *upper_dentry,
                                const char *buf, unsigned long size)
{
    struct file fake_file;
    unsigned long pos = 0;

    if (!upper_inode->i_fop || !upper_inode->i_fop->write)
        return -1;

    /* 构造临时 file 用于底层写入 */
    fake_file.f_dentry = upper_dentry;
    fake_file.f_inode = upper_inode;
    fake_file.f_op = upper_inode->i_fop;
    fake_file.f_flags = 1;     /* O_WRONLY */
    fake_file.f_pos = 0;
    fake_file.f_count = 1;

    return upper_inode->i_fop->write(&fake_file, buf, size, &pos);
}

/*
 * ============================================================
 * ovl_copy_up_one - copy-up 核心：将文件从 lower 复制到 upper
 *
 * 完整流程：
 *   1. 获取 lower 层文件数据（读取）
 *   2. 在 upper 层根目录创建同名文件
 *   3. 将数据写入 upper 层文件
 *   4. 复制元数据（mode、size）
 *   5. 更新 overlay inode 的 upper_dentry
 *
 * @sb:         overlay 超级块
 * @ovl_dentry: overlay dentry（包含文件名）
 *
 * 返回 0 成功，负数失败。
 *
 * 参考：fs/overlayfs/copy_up.c ovl_copy_up_one()
 * ============================================================
 */
int ovl_copy_up_one(struct super_block *sb, struct dentry *ovl_dentry)
{
    struct ovl_fs *ofs;
    struct ovl_inode_info *oi;
    struct inode *lower_inode;
    struct inode *upper_dir;
    struct dentry *upper_child;
    struct qstr qname;
    ssize_t data_size;
    ssize_t written;
    int ret;

    ofs = (struct ovl_fs *)sb->s_fs_info;
    if (!ofs)
        return -1;

    /* 获取 overlay inode info */
    if (!ovl_dentry->d_inode || !ovl_dentry->d_inode->i_private)
        return -1;

    oi = (struct ovl_inode_info *)ovl_dentry->d_inode->i_private;

    /* 验证 lower 层存在 */
    if (!oi->lower_dentry || !oi->lower_dentry->d_inode) {
        boot_printk("[overlayfs] copy-up: no lower inode\n");
        return -1;
    }
    lower_inode = oi->lower_dentry->d_inode;

    /* 验证 upper 层可写 */
    if (!ofs->upper_root || !ofs->upper_root->d_inode) {
        boot_printk("[overlayfs] copy-up: no upper root\n");
        return -1;
    }
    upper_dir = ofs->upper_root->d_inode;

    if (!upper_dir->i_op || !upper_dir->i_op->create) {
        boot_printk("[overlayfs] copy-up: upper doesn't support create\n");
        return -1;
    }

    boot_printk("[overlayfs] copy-up: ");
    boot_printk(ovl_dentry->d_name.name);
    boot_printk(" (size=");
    boot_printk_hex(lower_inode->i_size);
    boot_printk(")\n");

    /* Step 1: 从 lower 层读取文件数据 */
    data_size = 0;
    if (lower_inode->i_size > 0) {
        if (lower_inode->i_size > 4096) {
            boot_printk("[overlayfs] copy-up: file too large (max 4KB)\n");
            return -1;
        }

        data_size = ovl_copy_data(lower_inode, copyup_buf,
                                    lower_inode->i_size);
        if (data_size < 0) {
            boot_printk("[overlayfs] copy-up: read lower failed\n");
            return -1;
        }
    }

    /* Step 2: 在 upper 层创建同名文件 */
    qname.name = ovl_dentry->d_name.name;
    qname.len = ovl_dentry->d_name.len;
    qname.hash = full_name_hash(ofs->upper_root,
                                 ovl_dentry->d_name.name,
                                 ovl_dentry->d_name.len);

    upper_child = d_alloc(ofs->upper_root, &qname);
    if (!upper_child) {
        boot_printk("[overlayfs] copy-up: d_alloc failed\n");
        return -12;
    }

    ret = upper_dir->i_op->create(upper_dir, upper_child,
                                    lower_inode->i_mode, false);
    if (ret != 0) {
        boot_printk("[overlayfs] copy-up: create in upper failed\n");
        return ret;
    }

    /* Step 3: 将数据写入 upper 层 */
    if (data_size > 0 && upper_child->d_inode) {
        written = ovl_write_data(upper_child->d_inode,
                                  upper_child,
                                  copyup_buf,
                                  (unsigned long)data_size);
        if (written != data_size) {
            boot_printk("[overlayfs] copy-up: write upper failed\n");
            return -1;
        }
    }

    /* Step 4: 更新 overlay inode */
    oi->upper_dentry = upper_child;

    /* 更新 overlay inode 的 size（现在等于 upper 的） */
    if (upper_child->d_inode)
        ovl_dentry->d_inode->i_size = upper_child->d_inode->i_size;

    boot_printk("[overlayfs] copy-up complete: ");
    boot_printk(ovl_dentry->d_name.name);
    boot_printk(" (");
    boot_printk_hex((unsigned long)data_size);
    boot_printk(" bytes copied)\n");

    return 0;
}
