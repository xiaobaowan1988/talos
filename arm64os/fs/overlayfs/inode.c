/* SPDX-License-Identifier: GPL-2.0 */
/*
 * arm64os/fs/overlayfs/inode.c
 *
 * overlayfs inode 操作（路径查找、getattr）
 *
 * 参考：fs/overlayfs/inode.c
 *       fs/overlayfs/namei.c
 *
 * Phase 9 实现：
 *   - ovl_lookup()：在 upper 和 lower 层查找文件
 *   - ovl_alloc_inode_info()：分配 overlay inode 私有数据
 *   - ovl_create_inode()：创建 overlay inode 包装真实 inode
 *
 * 查找规则：
 *   1. 先在 upper 层查找
 *   2. 如果 upper 中是 whiteout（S_IFCHR），返回负 dentry
 *   3. 如果 upper 未找到，在 lower 层查找
 *   4. 创建 overlay inode 包装真实 inode
 */

#include <linux/types.h>
#include <linux/list.h>
#include <linux/fs.h>

/* 外部函数 */
void boot_printk(const char *s);
void boot_printk_hex(unsigned long val);

/* VFS 接口 */
struct inode *new_inode(struct super_block *sb);
struct dentry *d_alloc(struct dentry *parent, const struct qstr *name);
struct dentry *d_lookup(const struct dentry *parent, const struct qstr *name);
void d_add(struct dentry *dentry, struct inode *inode);
unsigned int full_name_hash(const void *salt, const char *name, unsigned int len);

/* copy-up 接口（由 copy_up.c 提供） */
int ovl_copy_up_one(struct super_block *sb, struct dentry *ovl_dentry);

/* overlayfs file 操作（由 file.c 提供） */
extern const struct file_operations ovl_file_fops;

/* whiteout 创建（由 dir.c 提供） */
int ovl_do_whiteout(struct super_block *sb, const char *name, unsigned int namelen);

/*
 * ============================================================
 * ovl_inode_info - overlay inode 私有数据
 *
 * 包装真实 inode 的来源信息（upper 层或 lower 层）。
 *
 * 参考：fs/overlayfs/ovl_entry.h struct ovl_inode
 * ============================================================
 */
struct ovl_inode_info {
    struct dentry  *upper_dentry;   /* upper 层 dentry（NULL 如果不在 upper） */
    struct dentry  *lower_dentry;   /* lower 层 dentry（NULL 如果不在 lower） */
};

/* ovl_inode_info 静态池 */
#define MAX_OVL_INODES  64
static struct ovl_inode_info ovl_info_pool[MAX_OVL_INODES];
static int ovl_info_idx = 0;

/*
 * ============================================================
 * ovl_fs - overlayfs 超级块私有数据（与 super.c 共用定义）
 * ============================================================
 */
struct ovl_fs {
    struct dentry  *lower_root;
    struct dentry  *upper_root;
    struct dentry  *work_root;
};

/*
 * ============================================================
 * ovl_alloc_inode_info - 分配 overlay inode 私有数据
 * ============================================================
 */
static struct ovl_inode_info *ovl_alloc_inode_info(void)
{
    struct ovl_inode_info *info;

    if (ovl_info_idx >= MAX_OVL_INODES) {
        boot_printk("[overlayfs] ERROR: inode info pool exhausted\n");
        return NULL;
    }

    info = &ovl_info_pool[ovl_info_idx++];
    info->upper_dentry = NULL;
    info->lower_dentry = NULL;

    return info;
}

/*
 * ============================================================
 * ovl_lookup_in_layer - 在指定层的根目录中查找子条目
 *
 * 通过 dcache 快速路径和文件系统 lookup 慢速路径查找。
 *
 * @layer_root: 该层的根 dentry
 * @name:       文件名
 * @namelen:    文件名长度
 *
 * 返回找到的 dentry（可能 d_inode=NULL），未找到返回 NULL。
 *
 * 参考：fs/overlayfs/namei.c ovl_lookup_single()
 * ============================================================
 */
static struct dentry *ovl_lookup_in_layer(struct dentry *layer_root,
                                           const char *name,
                                           unsigned int namelen)
{
    struct qstr qname;
    struct dentry *child;
    struct inode *dir_inode;

    if (!layer_root || !layer_root->d_inode)
        return NULL;

    /* 构造 qstr */
    qname.name = name;
    qname.len = namelen;
    qname.hash = full_name_hash(layer_root, name, namelen);

    /* 1. dcache 快速路径 */
    child = d_lookup(layer_root, &qname);
    if (child)
        return child;

    /* 2. 文件系统 lookup 慢速路径 */
    dir_inode = layer_root->d_inode;
    if (!dir_inode->i_op || !dir_inode->i_op->lookup)
        return NULL;

    child = d_alloc(layer_root, &qname);
    if (!child)
        return NULL;

    child = dir_inode->i_op->lookup(dir_inode, child, 0);
    return child;
}

/*
 * ============================================================
 * ovl_lookup - overlayfs 路径查找
 *
 * 在 upper 层和 lower 层中查找文件名，创建 overlay inode。
 *
 * 查找规则（参考 fs/overlayfs/namei.c ovl_lookup()）：
 *   1. 先在 upper 层查找
 *      - 如果找到 whiteout（S_IFCHR），文件已被删除，返回负 dentry
 *      - 如果找到正常文件，使用 upper 层版本
 *   2. 如果 upper 未找到，在 lower 层查找
 *   3. 创建 overlay inode 包装真实 inode，设置操作集
 *
 * @dir:    overlay 父目录 inode
 * @dentry: 待查找的 dentry（由 VFS 分配，name 已设置）
 * @flags:  查找标志
 *
 * 返回 dentry（找到则 d_inode 非 NULL），负 dentry 表示不存在。
 * ============================================================
 */
struct dentry *ovl_lookup(struct inode *dir, struct dentry *dentry,
                           unsigned int flags)
{
    struct ovl_fs *ofs;
    struct ovl_inode_info *oi;
    struct dentry *upper_child = NULL;
    struct dentry *lower_child = NULL;
    struct inode *ovl_inode;
    struct inode *real_inode = NULL;

    (void)flags;

    /*
     * 获取 ovl_fs：
     * 对于根目录，i_private 直接是 ovl_fs *。
     * 对于非根目录，需要从 sb->s_fs_info 获取。
     *
     * 当前简化版只支持平坦目录（不支持子目录嵌套），
     * 所以 dir 一定是 overlay 的根目录。
     */
    ofs = (struct ovl_fs *)dir->i_sb->s_fs_info;
    if (!ofs)
        return dentry;  /* 负 dentry */

    /* Step 1: 在 upper 层查找 */
    upper_child = ovl_lookup_in_layer(ofs->upper_root,
                                       dentry->d_name.name,
                                       dentry->d_name.len);

    if (upper_child && upper_child->d_inode) {
        /* 检查是否是 whiteout */
        if (S_ISCHR(upper_child->d_inode->i_mode)) {
            /*
             * whiteout：文件已被删除。
             * 返回负 dentry（d_inode = NULL）。
             */
            return dentry;
        }

        /* upper 层有此文件 */
        real_inode = upper_child->d_inode;
    }

    /* Step 2: 如果 upper 未找到，在 lower 层查找 */
    if (!real_inode) {
        lower_child = ovl_lookup_in_layer(ofs->lower_root,
                                           dentry->d_name.name,
                                           dentry->d_name.len);

        if (lower_child && lower_child->d_inode) {
            real_inode = lower_child->d_inode;
        }
    }

    /* 没有找到 */
    if (!real_inode)
        return dentry;  /* 负 dentry */

    /* Step 3: 创建 overlay inode */
    oi = ovl_alloc_inode_info();
    if (!oi)
        return dentry;

    oi->upper_dentry = (upper_child && upper_child->d_inode &&
                         !S_ISCHR(upper_child->d_inode->i_mode))
                        ? upper_child : NULL;
    oi->lower_dentry = (lower_child && lower_child->d_inode)
                        ? lower_child : NULL;

    ovl_inode = new_inode(dentry->d_sb);
    if (!ovl_inode)
        return dentry;

    /* 复制真实 inode 的元数据 */
    ovl_inode->i_mode = real_inode->i_mode;
    ovl_inode->i_size = real_inode->i_size;
    ovl_inode->i_private = oi;

    /* 设置操作集 */
    if (S_ISDIR(real_inode->i_mode)) {
        extern const struct inode_operations ovl_dir_inode_ops;
        extern const struct file_operations  ovl_dir_fops;
        ovl_inode->i_op = &ovl_dir_inode_ops;
        ovl_inode->i_fop = &ovl_dir_fops;
    } else {
        extern const struct inode_operations ovl_file_inode_ops;
        ovl_inode->i_op = &ovl_file_inode_ops;
        ovl_inode->i_fop = &ovl_file_fops;
    }

    /* 关联 inode 到 dentry 并加入 dcache */
    d_add(dentry, ovl_inode);

    return dentry;
}

/*
 * ============================================================
 * ovl_create - 在 overlay 中创建新文件
 *
 * 新文件直接创建在 upper 层。
 *
 * @dir:    overlay 父目录 inode
 * @dentry: 新文件的 dentry
 * @mode:   文件权限
 * @excl:   排他创建
 *
 * 参考：fs/overlayfs/dir.c ovl_create()
 * ============================================================
 */
static int ovl_create(struct inode *dir, struct dentry *dentry,
                        unsigned int mode, bool excl)
{
    struct ovl_fs *ofs;
    struct ovl_inode_info *oi;
    struct inode *upper_dir;
    struct dentry *upper_child;
    struct qstr qname;
    struct inode *ovl_inode;
    int ret;

    (void)excl;

    ofs = (struct ovl_fs *)dir->i_sb->s_fs_info;
    if (!ofs || !ofs->upper_root || !ofs->upper_root->d_inode)
        return -1;

    upper_dir = ofs->upper_root->d_inode;
    if (!upper_dir->i_op || !upper_dir->i_op->create)
        return -1;

    /* 在 upper 层分配 dentry */
    qname.name = dentry->d_name.name;
    qname.len = dentry->d_name.len;
    qname.hash = full_name_hash(ofs->upper_root,
                                 dentry->d_name.name,
                                 dentry->d_name.len);

    upper_child = d_alloc(ofs->upper_root, &qname);
    if (!upper_child)
        return -12;

    /* 在 upper 层创建文件 */
    ret = upper_dir->i_op->create(upper_dir, upper_child, mode, false);
    if (ret != 0)
        return ret;

    /* 创建 overlay inode 包装 */
    oi = ovl_alloc_inode_info();
    if (!oi)
        return -12;

    oi->upper_dentry = upper_child;
    oi->lower_dentry = NULL;

    ovl_inode = new_inode(dentry->d_sb);
    if (!ovl_inode)
        return -12;

    ovl_inode->i_mode = upper_child->d_inode->i_mode;
    ovl_inode->i_size = upper_child->d_inode->i_size;
    ovl_inode->i_private = oi;

    {
        extern const struct inode_operations ovl_file_inode_ops;
        ovl_inode->i_op = &ovl_file_inode_ops;
    }
    ovl_inode->i_fop = &ovl_file_fops;

    d_add(dentry, ovl_inode);

    return 0;
}

/*
 * ============================================================
 * ovl_unlink - 删除 overlay 中的文件
 *
 * 如果文件在 upper 层：直接从 upper 删除。
 * 如果文件只在 lower 层：在 upper 层创建 whiteout。
 * 如果文件在两层都有（copy-up 过的）：删除 upper 并创建 whiteout。
 *
 * @dir:    overlay 父目录 inode
 * @dentry: 要删除的 overlay dentry
 *
 * 参考：fs/overlayfs/dir.c ovl_unlink()
 * ============================================================
 */
static int ovl_unlink(struct inode *dir, struct dentry *dentry)
{
    struct ovl_inode_info *oi;
    struct ovl_fs *ofs;
    int need_whiteout;

    if (!dentry->d_inode)
        return -2;  /* ENOENT */

    oi = (struct ovl_inode_info *)dentry->d_inode->i_private;
    ofs = (struct ovl_fs *)dir->i_sb->s_fs_info;

    if (!oi || !ofs)
        return -1;

    /*
     * 需要 whiteout 的情况：
     * - 文件在 lower 层存在（需要屏蔽 lower 层的文件）
     */
    need_whiteout = (oi->lower_dentry != NULL);

    if (need_whiteout) {
        /* 在 upper 层创建 whiteout */
        int ret = ovl_do_whiteout(dir->i_sb,
                                   dentry->d_name.name,
                                   dentry->d_name.len);
        if (ret != 0)
            return ret;
    }

    /* 使 overlay dentry 变为负 dentry（文件"消失"） */
    dentry->d_inode = NULL;

    boot_printk("[overlayfs] unlink: ");
    boot_printk(dentry->d_name.name);
    if (need_whiteout)
        boot_printk(" (whiteout created)");
    boot_printk("\n");

    return 0;
}

/*
 * ============================================================
 * 操作集定义
 * ============================================================
 */

/* overlayfs 目录 inode 操作集 */
const struct inode_operations ovl_dir_inode_ops = {
    .lookup     = ovl_lookup,
    .create     = ovl_create,
    .mkdir      = NULL,         /* Phase 9 不支持 mkdir on overlay */
    .unlink     = ovl_unlink,
};

/* overlayfs 文件 inode 操作集 */
const struct inode_operations ovl_file_inode_ops = {
    .lookup     = NULL,
    .create     = NULL,
    .mkdir      = NULL,
    .unlink     = NULL,
};

/* overlayfs 目录 file 操作集 */
const struct file_operations ovl_dir_fops = {
    .read       = NULL,
    .write      = NULL,
    .open       = NULL,
    .release    = NULL,
};
