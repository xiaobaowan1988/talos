/* SPDX-License-Identifier: GPL-2.0 */
/*
 * arm64os/fs/vfs/namei.c
 *
 * 路径名解析（pathname lookup）
 *
 * 参考：fs/namei.c
 *
 * Phase 7 实现：
 *   - path_lookup()：解析路径名为 dentry
 *   - path_lookup_create()：解析路径名，返回父 dentry 和末尾分量
 *   - walk_component()：解析路径中的一个分量
 *
 * 路径解析是 VFS 最核心的操作之一：
 *   open("/etc/passwd") →
 *     1. 从根 dentry 开始
 *     2. 解析 "etc"：dcache lookup → 如果 miss 则 inode->i_op->lookup()
 *     3. 解析 "passwd"：同上
 *     4. 返回 "passwd" 的 dentry
 *
 * 简化说明：
 *   - 不处理符号链接（Phase 7 无 symlink 支持）
 *   - 不检查权限（Phase 7 无用户 ID 支持）
 *   - 只处理绝对路径（从根目录开始）
 *   - 不支持 ".." 和 "."（Phase 7 简化）— 实际支持 "."
 */

#include <linux/types.h>
#include <linux/list.h>
#include <linux/fs.h>

/* 外部函数 */
void boot_printk(const char *s);
void boot_printk_hex(unsigned long val);

/* 全局根挂载点 */
extern struct vfsmount *root_mnt;

/*
 * ============================================================
 * 内部辅助函数
 * ============================================================
 */

/*
 * skip_slashes - 跳过路径中连续的 '/'
 */
static const char *skip_slashes(const char *path)
{
    while (*path == '/')
        path++;
    return path;
}

/*
 * next_component - 提取下一个路径分量
 *
 * 从 path 中提取到下一个 '/' 或 '\0' 之前的子串。
 *
 * @path:    当前路径位置
 * @name_out: 输出分量名
 * @len_out:  输出分量长度
 *
 * 返回下一个分量的起始位置（跳过了分隔的 '/'）。
 */
static const char *next_component(const char *path, const char **name_out,
                                   unsigned int *len_out)
{
    const char *start = path;
    unsigned int len = 0;

    while (*path && *path != '/') {
        len++;
        path++;
    }

    *name_out = start;
    *len_out = len;

    /* 跳过尾部的 '/' */
    return skip_slashes(path);
}

/*
 * ============================================================
 * walk_component - 解析一个路径分量
 *
 * 在 parent 目录下查找名为 name 的子条目。
 *
 * 查找顺序：
 *   1. dcache 快速路径（d_lookup）
 *   2. 如果 miss，调用文件系统的 lookup 回调（慢速路径）
 *
 * @parent: 当前目录 dentry
 * @name:   分量名
 * @len:    分量长度
 *
 * 返回子条目 dentry，未找到返回 NULL。
 *
 * 参考：fs/namei.c walk_component() → lookup_fast() / lookup_slow()
 * ============================================================
 */
static struct dentry *walk_component(struct dentry *parent,
                                      const char *name, unsigned int len)
{
    struct qstr qname;
    struct dentry *dentry;
    struct inode *dir_inode;

    /* 处理 "."（当前目录）*/
    if (len == 1 && name[0] == '.') {
        return dget(parent);
    }

    /* 处理 ".."（父目录）*/
    if (len == 2 && name[0] == '.' && name[1] == '.') {
        return dget(parent->d_parent);
    }

    /* 构造 qstr */
    qname.name = name;
    qname.len = len;
    qname.hash = full_name_hash(parent, name, len);

    /* 1. 快速路径：dcache 查找 */
    dentry = d_lookup(parent, &qname);
    if (dentry)
        return dentry;      /* Cache hit */

    /* 2. 慢速路径：调用文件系统 lookup */
    dir_inode = parent->d_inode;
    if (!dir_inode || !dir_inode->i_op || !dir_inode->i_op->lookup)
        return NULL;

    /* 分配一个新的负 dentry，交给文件系统 lookup 填充 */
    dentry = d_alloc(parent, &qname);
    if (!dentry)
        return NULL;

    /* 文件系统 lookup：查找 inode 并关联到 dentry */
    dentry = dir_inode->i_op->lookup(dir_inode, dentry, 0);

    return dentry;
}

/*
 * ============================================================
 * path_lookup - 解析完整路径名为 dentry
 *
 * 将路径名（如 "/etc/passwd"）解析为对应的 dentry。
 * 逐级解析路径分量，每一级先查 dcache，miss 则调用文件系统 lookup。
 *
 * @pathname: 路径名（必须是绝对路径）
 *
 * 返回目标 dentry（引用计数 +1），路径无效返回 NULL。
 *
 * 参考：fs/namei.c filename_lookup() → path_lookupat()
 * ============================================================
 */
struct dentry *path_lookup(const char *pathname)
{
    struct dentry *dentry;
    const char *path;
    const char *name;
    unsigned int len;

    /* 必须有根文件系统 */
    if (!root_mnt || !root_mnt->mnt_root)
        return NULL;

    /* 绝对路径从根 dentry 开始 */
    if (*pathname != '/')
        return NULL;    /* Phase 7 不支持相对路径 */

    dentry = root_mnt->mnt_root;

    /* 跳过开头的 '/' */
    path = skip_slashes(pathname);

    /* 路径为 "/" — 直接返回根 dentry */
    if (*path == '\0')
        return dget(dentry);

    /* 逐级解析路径分量 */
    while (*path) {
        struct dentry *next;

        path = next_component(path, &name, &len);

        if (len == 0)
            break;

        next = walk_component(dentry, name, len);
        if (!next)
            return NULL;    /* 路径分量不存在 */

        dentry = next;
    }

    return dentry;  /* walk_component 已增加引用计数 */
}

/*
 * ============================================================
 * path_lookup_create - 解析路径并为创建做准备
 *
 * 解析路径的父目录部分，返回父 dentry 和最后一个路径分量。
 * 用于 open(O_CREAT)：先找到父目录，再在其中创建新文件。
 *
 * @pathname:    完整路径名
 * @parent_out:  输出父目录 dentry
 * @last_out:    输出最后一个路径分量（文件名）
 *
 * 返回：分配的新 dentry（未关联 inode），失败返回 NULL。
 *
 * 参考：fs/namei.c filename_create()
 * ============================================================
 */
struct dentry *path_lookup_create(const char *pathname,
                                   struct dentry **parent_out,
                                   struct qstr *last_out)
{
    struct dentry *parent;
    struct dentry *dentry;
    const char *path;
    const char *name;
    unsigned int len;
    const char *prev_name = NULL;
    unsigned int prev_len = 0;

    /* 必须有根文件系统 */
    if (!root_mnt || !root_mnt->mnt_root)
        return NULL;

    if (*pathname != '/')
        return NULL;

    parent = root_mnt->mnt_root;
    path = skip_slashes(pathname);

    if (*path == '\0')
        return NULL;    /* "/" — 不能创建根 */

    /*
     * 逐级解析路径，直到最后一个分量。
     * 最后一个分量是要创建的文件名，其之前的部分是父目录路径。
     */
    while (*path) {
        prev_name = NULL;
        path = next_component(path, &name, &len);

        if (len == 0)
            break;

        if (*path == '\0') {
            /* 这是最后一个分量（要创建的文件名） */
            prev_name = name;
            prev_len = len;
            break;
        }

        /* 解析中间目录分量 */
        {
            struct dentry *next = walk_component(parent, name, len);
            if (!next) {
                /* 中间目录不存在 */
                return NULL;
            }
            parent = next;
        }
    }

    if (!prev_name)
        return NULL;

    /* 构造最后分量的 qstr */
    last_out->name = prev_name;
    last_out->len = prev_len;
    last_out->hash = full_name_hash(parent, prev_name, prev_len);

    *parent_out = parent;

    /* 分配 dentry（待调用者通过 create 填充 inode）*/
    dentry = d_alloc(parent, last_out);

    return dentry;
}
