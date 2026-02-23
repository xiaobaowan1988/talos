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
 * Phase 8 增强：
 *   - path_lookup() 在解析前检查挂载表
 *   - 如果路径匹配某个非根挂载点，从该挂载点的 dentry 树开始解析
 *   - 例如 "/sq/hello.txt" → 在 squashfs 的 dentry 树中查找 "hello.txt"
 *
 * 简化说明：
 *   - 不处理符号链接
 *   - 不检查权限
 *   - 只处理绝对路径
 */

#include <linux/types.h>
#include <linux/list.h>
#include <linux/fs.h>

/* 外部函数 */
void boot_printk(const char *s);
void boot_printk_hex(unsigned long val);

/* 全局根挂载点 */
extern struct vfsmount *root_mnt;
extern struct mount_entry mount_table[MAX_MOUNTS];

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
 * 内部辅助：字符串前缀比较
 * ============================================================
 */
static int path_starts_with(const char *path, const char *prefix, int prefix_len)
{
    int i;
    for (i = 0; i < prefix_len; i++) {
        if (path[i] != prefix[i])
            return 0;
    }
    return 1;
}

/*
 * ============================================================
 * resolve_mount_point - 解析挂载点
 *
 * 检查路径是否匹配某个非根挂载点。如果匹配，返回该挂载点的
 * root dentry 和路径剩余部分。
 *
 * @pathname:   完整路径
 * @dentry_out: 输出起始 dentry
 * @rest_out:   输出路径剩余部分（跳过挂载前缀后）
 *
 * 返回 1 表示匹配了非根挂载点，0 表示使用根文件系统。
 * ============================================================
 */
static int resolve_mount_point(const char *pathname,
                                struct dentry **dentry_out,
                                const char **rest_out)
{
    struct mount_entry *best = NULL;
    int best_len = 0;
    int plen;
    int i;

    /* 计算路径长度 */
    plen = 0;
    while (pathname[plen])
        plen++;

    /* 查找最长前缀匹配的挂载点 */
    for (i = 0; i < MAX_MOUNTS; i++) {
        int mlen;

        if (!mount_table[i].used)
            continue;

        mlen = mount_table[i].mnt_pathlen;

        /* 跳过根挂载 */
        if (mlen == 1 && mount_table[i].mnt_path[0] == '/')
            continue;

        /* 路径长度检查 */
        if (plen < mlen)
            continue;

        /* 前缀匹配 */
        if (!path_starts_with(pathname, mount_table[i].mnt_path, mlen))
            continue;

        /* 挂载路径后必须是 '/' 或 '\0' */
        if (plen > mlen && pathname[mlen] != '/')
            continue;

        /* 选择最长匹配 */
        if (mlen > best_len) {
            best = &mount_table[i];
            best_len = mlen;
        }
    }

    if (best) {
        *dentry_out = best->mnt.mnt_root;
        /* 计算剩余路径 */
        if (pathname[best_len] == '/')
            *rest_out = skip_slashes(pathname + best_len);
        else
            *rest_out = pathname + best_len; /* '\0' */
        return 1;
    }

    return 0;
}

/*
 * ============================================================
 * path_lookup - 解析完整路径名为 dentry
 *
 * 将路径名（如 "/etc/passwd"）解析为对应的 dentry。
 *
 * Phase 8 增强：
 *   先检查挂载表，如果路径匹配非根挂载点（如 "/sq"），
 *   则从该挂载点的 root dentry 开始解析剩余路径。
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
        return NULL;    /* 不支持相对路径 */

    /*
     * Phase 8：检查挂载表
     * 如果路径匹配非根挂载点，从该挂载点的 dentry 树开始
     */
    {
        struct dentry *mnt_root;
        const char *rest;

        if (resolve_mount_point(pathname, &mnt_root, &rest)) {
            dentry = mnt_root;
            path = rest;

            /* 如果剩余路径为空，直接返回挂载点根 */
            if (*path == '\0')
                return dget(dentry);

            /* 从挂载点根开始解析剩余路径 */
            goto resolve_rest;
        }
    }

    /* 使用根文件系统 */
    dentry = root_mnt->mnt_root;

    /* 跳过开头的 '/' */
    path = skip_slashes(pathname);

    /* 路径为 "/" — 直接返回根 dentry */
    if (*path == '\0')
        return dget(dentry);

resolve_rest:
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
 * Phase 8 增强：支持在非根挂载点中创建文件。
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

    /*
     * Phase 8：检查挂载表
     */
    {
        struct dentry *mnt_root;
        const char *rest;

        if (resolve_mount_point(pathname, &mnt_root, &rest)) {
            parent = mnt_root;
            path = rest;

            if (*path == '\0')
                return NULL; /* 不能在挂载根上创建 */

            goto resolve_create;
        }
    }

    parent = root_mnt->mnt_root;
    path = skip_slashes(pathname);

    if (*path == '\0')
        return NULL;    /* "/" — 不能创建根 */

resolve_create:
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
