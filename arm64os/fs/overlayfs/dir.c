/* SPDX-License-Identifier: GPL-2.0 */
/*
 * arm64os/fs/overlayfs/dir.c
 *
 * overlayfs 目录操作、whiteout 处理
 *
 * 参考：fs/overlayfs/dir.c
 *
 * Phase 9 实现：
 *   - ovl_do_whiteout()：在 upper 层创建 whiteout 文件
 *   - ovl_is_whiteout()：检查 dentry 是否是 whiteout
 *
 * whiteout 原理：
 *   - overlayfs 无法修改 lower 层（只读）
 *   - 删除文件时，在 upper 层创建同名的 whiteout 特殊文件
 *   - whiteout 是字符设备文件，主/次设备号 (0, 0)
 *   - 路径查找遇到 whiteout 即视为文件不存在
 *   - 实际 Linux 内核使用 mknod 创建 S_IFCHR 设备节点
 *
 * 简化说明：
 *   - whiteout 通过 ramfs_create + 设置 i_mode = S_IFCHR 实现
 *   - 不支持目录级 whiteout（opaque 标记）
 *   - 不支持目录合并 readdir（Phase 9 简化版）
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
 * ============================================================
 * whiteout 常量
 *
 * 参考：include/linux/fs.h, fs/overlayfs/overlayfs.h
 * ============================================================
 */
#define WHITEOUT_MODE       (S_IFCHR)       /* 字符设备类型 */
#define WHITEOUT_DEV        0               /* 设备号 (0, 0) */

/*
 * ovl_fs 定义（与 super.c 共用）
 */
struct ovl_fs {
    struct dentry  *lower_root;
    struct dentry  *upper_root;
    struct dentry  *work_root;
};

/*
 * ============================================================
 * ovl_is_whiteout - 检查 dentry 是否是 whiteout
 *
 * whiteout 判定条件：
 *   - inode 类型为 S_IFCHR（字符设备）
 *   - 在真实 Linux 中还检查 MAJOR(rdev)==0 && MINOR(rdev)==0
 *   - 我们的简化版只检查 S_IFCHR
 *
 * @dentry: 要检查的 dentry
 *
 * 返回 1 是 whiteout，0 不是。
 *
 * 参考：fs/overlayfs/util.c ovl_is_whiteout()
 * ============================================================
 */
int ovl_is_whiteout(struct dentry *dentry)
{
    if (!dentry || !dentry->d_inode)
        return 0;

    return S_ISCHR(dentry->d_inode->i_mode);
}

/*
 * ============================================================
 * ovl_do_whiteout - 在 upper 层创建 whiteout 文件
 *
 * 流程：
 *   1. 在 upper 层的根目录下分配 dentry
 *   2. 通过 upper 层文件系统的 create 创建文件
 *   3. 将 inode 的 mode 设置为 S_IFCHR（字符设备）
 *
 * 这样 overlayfs 在 lookup 时遇到此 inode，
 * ovl_is_whiteout() 返回 true，文件视为已删除。
 *
 * @sb:      overlay 超级块
 * @name:    文件名
 * @namelen: 文件名长度
 *
 * 返回 0 成功，负数失败。
 *
 * 参考：fs/overlayfs/dir.c ovl_whiteout()
 * ============================================================
 */
int ovl_do_whiteout(struct super_block *sb, const char *name,
                      unsigned int namelen)
{
    struct ovl_fs *ofs;
    struct inode *upper_dir;
    struct dentry *wh_dentry;
    struct qstr qname;
    int ret;

    ofs = (struct ovl_fs *)sb->s_fs_info;
    if (!ofs || !ofs->upper_root || !ofs->upper_root->d_inode)
        return -1;

    upper_dir = ofs->upper_root->d_inode;
    if (!upper_dir->i_op || !upper_dir->i_op->create)
        return -1;

    /* 检查 upper 层是否已有同名文件 */
    qname.name = name;
    qname.len = namelen;
    qname.hash = full_name_hash(ofs->upper_root, name, namelen);

    wh_dentry = d_lookup(ofs->upper_root, &qname);

    if (wh_dentry && wh_dentry->d_inode) {
        /*
         * upper 层已有此文件（可能是 copy-up 过的）。
         * 直接将其转为 whiteout：修改 inode mode。
         */
        wh_dentry->d_inode->i_mode = WHITEOUT_MODE;
        wh_dentry->d_inode->i_size = 0;
        return 0;
    }

    /* upper 层无此文件，创建新的 whiteout */
    wh_dentry = d_alloc(ofs->upper_root, &qname);
    if (!wh_dentry)
        return -12;

    /*
     * 使用 upper 层文件系统的 create 创建文件。
     * 传入 S_IFREG 让 ramfs_create 正常工作（分配 inode + 数据结构），
     * 然后我们手动修改 mode 为 S_IFCHR。
     */
    ret = upper_dir->i_op->create(upper_dir, wh_dentry,
                                    S_IFREG | 0000, false);
    if (ret != 0)
        return ret;

    /* 将 mode 改为字符设备（whiteout 标记） */
    if (wh_dentry->d_inode) {
        wh_dentry->d_inode->i_mode = WHITEOUT_MODE;
        wh_dentry->d_inode->i_size = 0;
        wh_dentry->d_inode->i_fop = NULL;  /* whiteout 不可读写 */
    }

    boot_printk("[overlayfs] whiteout created: ");
    {
        unsigned int i;
        char nbuf[32];
        for (i = 0; i < namelen && i < 31; i++)
            nbuf[i] = name[i];
        nbuf[i] = '\0';
        boot_printk(nbuf);
    }
    boot_printk("\n");

    return 0;
}
