/* SPDX-License-Identifier: GPL-2.0 */
/*
 * arm64os/fs/ramfs/ramfs.c
 *
 * 简单 RAM 文件系统（内存文件系统）
 *
 * 参考：fs/ramfs/inode.c
 *       mm/shmem.c（tmpfs 基础）
 *
 * Phase 7 实现：
 *   - ramfs 将文件内容存储在内存中（无磁盘后端）
 *   - 支持创建普通文件和目录
 *   - 文件数据使用 Buddy 分配器分配的页面存储
 *   - 用于 VFS 功能测试
 *
 * 支持的操作：
 *   - create：创建普通文件
 *   - mkdir：创建目录
 *   - lookup：查找目录项
 *   - read/write：文件 I/O
 *
 * 简化说明：
 *   - 每个文件最大 4KB（单页）
 *   - 目录条目通过 dentry 子链表管理
 *   - 无权限检查
 */

#include <linux/types.h>
#include <linux/list.h>
#include <linux/fs.h>
#include <asm/memory.h>

/* 外部函数 */
void boot_printk(const char *s);
void boot_printk_hex(unsigned long val);

/* Buddy 分配器 */
struct page;
struct page *alloc_pages(unsigned int order);
void *page_address(struct page *page);

/* VFS 接口 */
struct super_block *alloc_super(struct file_system_type *type);
struct inode *new_inode(struct super_block *sb);
struct dentry *d_alloc_root(struct super_block *sb);
void d_add(struct dentry *dentry, struct inode *inode);
int register_filesystem(struct file_system_type *fs);

/*
 * ============================================================
 * ramfs 文件系统魔数
 * ============================================================
 */
#define RAMFS_MAGIC     0x858458f6UL    /* 与 Linux ramfs 一致 */

/*
 * ============================================================
 * ramfs_inode_info - ramfs 文件私有数据
 *
 * 对于普通文件：data 指向存储文件内容的内存页。
 * 对于目录：data = NULL（子条目通过 dentry 管理）。
 *
 * 参考：fs/ramfs/inode.c（Linux ramfs 使用 page cache，此处简化）
 * ============================================================
 */
struct ramfs_inode_info {
    char    *data;          /* 文件数据缓冲区（页对齐）*/
    size_t   capacity;      /* 已分配容量（字节）*/
};

/* ramfs inode info 静态池 */
#define MAX_RAMFS_INODES    64
static struct ramfs_inode_info ramfs_info_pool[MAX_RAMFS_INODES];
static int ramfs_info_idx = 0;

/*
 * ============================================================
 * 前向声明
 * ============================================================
 */
static const struct inode_operations ramfs_dir_inode_ops;
static const struct inode_operations ramfs_file_inode_ops;
static const struct file_operations  ramfs_file_ops;
static const struct file_operations  ramfs_dir_ops;
static const struct super_operations ramfs_super_ops;

/*
 * ============================================================
 * ramfs_mem_copy - 内存复制
 * ============================================================
 */
static void ramfs_mem_copy(char *dst, const char *src, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++)
        dst[i] = src[i];
}

/*
 * ============================================================
 * ramfs_mem_zero - 内存清零
 * ============================================================
 */
static void ramfs_mem_zero(char *dst, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++)
        dst[i] = 0;
}

/*
 * ============================================================
 * ramfs_alloc_inode_info - 分配 ramfs inode 私有数据
 * ============================================================
 */
static struct ramfs_inode_info *ramfs_alloc_inode_info(void)
{
    struct ramfs_inode_info *info;

    if (ramfs_info_idx >= MAX_RAMFS_INODES)
        return NULL;

    info = &ramfs_info_pool[ramfs_info_idx++];
    info->data = NULL;
    info->capacity = 0;

    return info;
}

/*
 * ============================================================
 * ramfs_get_inode - 创建 ramfs inode
 *
 * 分配 inode 和 ramfs 私有数据，设置操作集。
 *
 * @sb:   超级块
 * @mode: 文件类型 + 权限
 *
 * 返回 inode 指针。
 *
 * 参考：fs/ramfs/inode.c ramfs_get_inode()
 * ============================================================
 */
static struct inode *ramfs_get_inode(struct super_block *sb, unsigned int mode)
{
    struct inode *inode;
    struct ramfs_inode_info *info;

    inode = new_inode(sb);
    if (!inode)
        return NULL;

    info = ramfs_alloc_inode_info();
    if (!info)
        return NULL;

    inode->i_mode = mode;
    inode->i_size = 0;
    inode->i_private = info;

    if (S_ISDIR(mode)) {
        /* 目录 */
        inode->i_op = &ramfs_dir_inode_ops;
        inode->i_fop = &ramfs_dir_ops;
    } else if (S_ISREG(mode)) {
        /* 普通文件 */
        inode->i_op = &ramfs_file_inode_ops;
        inode->i_fop = &ramfs_file_ops;
    }

    return inode;
}

/*
 * ============================================================
 * ramfs_create - 在目录中创建普通文件
 *
 * @dir:    父目录 inode
 * @dentry: 新文件的 dentry（已分配，待关联 inode）
 * @mode:   文件权限
 * @excl:   是否要求排他创建
 *
 * 参考：fs/ramfs/inode.c ramfs_mknod()
 * ============================================================
 */
static int ramfs_create(struct inode *dir, struct dentry *dentry,
                         unsigned int mode, bool excl)
{
    struct inode *inode;

    inode = ramfs_get_inode(dir->i_sb, mode);
    if (!inode)
        return -12; /* ENOMEM */

    /* 将 inode 关联到 dentry 并加入 dcache */
    d_add(dentry, inode);

    return 0;
}

/*
 * ============================================================
 * ramfs_mkdir - 在目录中创建子目录
 *
 * @dir:    父目录 inode
 * @dentry: 新目录的 dentry
 * @mode:   目录权限
 *
 * 参考：fs/ramfs/inode.c ramfs_mkdir()
 * ============================================================
 */
static int ramfs_mkdir(struct inode *dir, struct dentry *dentry,
                        unsigned int mode)
{
    struct inode *inode;

    inode = ramfs_get_inode(dir->i_sb, S_IFDIR | (mode & 0777));
    if (!inode)
        return -12; /* ENOMEM */

    d_add(dentry, inode);

    return 0;
}

/*
 * ============================================================
 * ramfs_lookup - 在目录中查找条目
 *
 * dcache 未命中时由 VFS 调用。
 * ramfs 的目录内容完全通过 dentry 子链表管理，
 * 如果 dcache miss 说明文件不存在。
 *
 * @dir:    目录 inode
 * @dentry: 待查找的 dentry（已分配，name 已设置）
 * @flags:  查找标志
 *
 * 返回 dentry（如果找到），NULL 表示不存在。
 *
 * 参考：fs/ramfs/inode.c → simple_lookup()
 * ============================================================
 */
static struct dentry *ramfs_lookup(struct inode *dir, struct dentry *dentry,
                                    unsigned int flags)
{
    /*
     * ramfs 的所有条目都通过 d_add 加入了 dcache。
     * 如果 d_lookup 未命中到达这里，说明文件确实不存在。
     * 返回传入的 dentry（d_inode = NULL，作为负 dentry 缓存）。
     */
    (void)dir;
    (void)flags;
    return dentry;
}

/*
 * ============================================================
 * ramfs_read - 读取文件数据
 *
 * @filp:  file 对象
 * @buf:   目标缓冲区
 * @count: 请求读取的字节数
 * @pos:   读写偏移指针
 *
 * 返回实际读取的字节数。
 *
 * 参考：fs/ramfs（实际通过 page cache，此处简化为直接内存拷贝）
 * ============================================================
 */
static ssize_t ramfs_read(struct file *filp, char *buf, size_t count,
                           unsigned long *pos)
{
    struct inode *inode = filp->f_inode;
    struct ramfs_inode_info *info;
    size_t avail;

    if (!inode || !inode->i_private)
        return 0;

    info = (struct ramfs_inode_info *)inode->i_private;

    /* 没有数据 */
    if (!info->data || inode->i_size == 0)
        return 0;

    /* 计算可读字节数 */
    if (*pos >= inode->i_size)
        return 0;   /* 已到文件末尾 */

    avail = inode->i_size - *pos;
    if (count > avail)
        count = avail;

    /* 复制数据 */
    ramfs_mem_copy(buf, info->data + *pos, count);
    *pos += count;

    return (ssize_t)count;
}

/*
 * ============================================================
 * ramfs_write - 写入文件数据
 *
 * @filp:  file 对象
 * @buf:   源数据缓冲区
 * @count: 写入字节数
 * @pos:   读写偏移指针
 *
 * 返回实际写入的字节数。
 *
 * 如果文件尚未分配数据页，首次写入时分配一页（4KB）。
 * 超过页大小的写入被截断到页边界。
 *
 * 参考：fs/ramfs（简化版直接内存写入）
 * ============================================================
 */
static ssize_t ramfs_write(struct file *filp, const char *buf, size_t count,
                            unsigned long *pos)
{
    struct inode *inode = filp->f_inode;
    struct ramfs_inode_info *info;
    struct page *page;
    size_t end;

    if (!inode || !inode->i_private)
        return -9;  /* EBADF */

    info = (struct ramfs_inode_info *)inode->i_private;

    /* 首次写入：分配数据页 */
    if (!info->data) {
        page = alloc_pages(0);  /* 1 页 = 4KB */
        if (!page) {
            boot_printk("[ramfs] ERROR: cannot allocate data page\n");
            return -12; /* ENOMEM */
        }
        info->data = (char *)page_address(page);
        info->capacity = PAGE_SIZE;
        ramfs_mem_zero(info->data, PAGE_SIZE);
    }

    /* 检查写入范围是否超过容量 */
    end = *pos + count;
    if (end > info->capacity)
        count = info->capacity - *pos;

    if (count == 0)
        return -28; /* ENOSPC */

    /* 写入数据 */
    ramfs_mem_copy(info->data + *pos, buf, count);
    *pos += count;

    /* 更新文件大小 */
    if (*pos > inode->i_size)
        inode->i_size = *pos;

    return (ssize_t)count;
}

/*
 * ============================================================
 * 操作集定义
 * ============================================================
 */

/* 目录 inode 操作集 */
static const struct inode_operations ramfs_dir_inode_ops = {
    .lookup     = ramfs_lookup,
    .create     = ramfs_create,
    .mkdir      = ramfs_mkdir,
    .unlink     = NULL,
};

/* 文件 inode 操作集（普通文件无需 lookup/create）*/
static const struct inode_operations ramfs_file_inode_ops = {
    .lookup     = NULL,
    .create     = NULL,
    .mkdir      = NULL,
    .unlink     = NULL,
};

/* 文件操作集 */
static const struct file_operations ramfs_file_ops = {
    .read       = ramfs_read,
    .write      = ramfs_write,
    .open       = NULL,     /* 无需特殊 open 处理 */
    .release    = NULL,     /* 无需特殊 close 处理 */
};

/* 目录文件操作集（Phase 7 不支持 readdir）*/
static const struct file_operations ramfs_dir_ops = {
    .read       = NULL,
    .write      = NULL,
    .open       = NULL,
    .release    = NULL,
};

/* 超级块操作集 */
static const struct super_operations ramfs_super_ops = {
    .alloc_inode    = NULL,     /* 使用默认 new_inode */
    .destroy_inode  = NULL,
    .put_super      = NULL,
};

/*
 * ============================================================
 * ramfs_fill_super - 填充超级块
 *
 * 创建根目录 inode 和 dentry。
 *
 * @sb: 已分配的超级块
 *
 * 返回 0 成功。
 *
 * 参考：fs/ramfs/inode.c ramfs_fill_super()
 * ============================================================
 */
static int ramfs_fill_super(struct super_block *sb)
{
    struct inode *root_inode;
    struct dentry *root_dentry;

    sb->s_magic = RAMFS_MAGIC;
    sb->s_blocksize = PAGE_SIZE;
    sb->s_op = &ramfs_super_ops;

    /* 创建根目录 inode */
    root_inode = ramfs_get_inode(sb, S_IFDIR | 0755);
    if (!root_inode) {
        boot_printk("[ramfs] ERROR: cannot create root inode\n");
        return -12;
    }

    /* 创建根目录 dentry */
    root_dentry = d_alloc_root(sb);
    if (!root_dentry) {
        boot_printk("[ramfs] ERROR: cannot create root dentry\n");
        return -12;
    }

    /* 关联 inode 到 dentry */
    root_dentry->d_inode = root_inode;
    sb->s_root = root_dentry;

    boot_printk("[ramfs] Root inode created (ino=");
    boot_printk_hex(root_inode->i_ino);
    boot_printk(")\n");

    return 0;
}

/*
 * ============================================================
 * ramfs_mount - 挂载 ramfs 文件系统
 *
 * file_system_type 的 mount 回调。
 * 分配超级块，填充根目录，返回根 dentry。
 *
 * 参考：fs/ramfs/inode.c ramfs_mount()
 * ============================================================
 */
static struct dentry *ramfs_mount(struct file_system_type *fs_type,
                                   int flags, const char *dev_name, void *data)
{
    struct super_block *sb;
    int ret;

    boot_printk("[ramfs] Mounting ramfs...\n");

    sb = alloc_super(fs_type);
    if (!sb)
        return NULL;

    ret = ramfs_fill_super(sb);
    if (ret != 0)
        return NULL;

    boot_printk("[ramfs] ramfs mounted successfully\n");

    return sb->s_root;
}

/*
 * ============================================================
 * ramfs_kill_sb - 卸载 ramfs
 *
 * Phase 7 简化：不实际回收资源。
 * ============================================================
 */
static void ramfs_kill_sb(struct super_block *sb)
{
    boot_printk("[ramfs] Unmounting ramfs\n");
    (void)sb;
}

/*
 * ============================================================
 * ramfs 文件系统类型
 * ============================================================
 */
static struct file_system_type ramfs_fs_type = {
    .name       = "ramfs",
    .mount      = ramfs_mount,
    .kill_sb    = ramfs_kill_sb,
    .next       = NULL,
};

/*
 * ============================================================
 * ramfs_init - 注册 ramfs 文件系统
 *
 * 在 VFS 初始化后调用。
 *
 * 参考：fs/ramfs/inode.c init_ramfs_fs()
 * ============================================================
 */
void ramfs_init(void)
{
    register_filesystem(&ramfs_fs_type);
}
