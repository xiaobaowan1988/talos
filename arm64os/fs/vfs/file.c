/* SPDX-License-Identifier: GPL-2.0 */
/*
 * arm64os/fs/vfs/file.c
 *
 * 文件对象与文件描述符表管理
 *
 * 参考：fs/file.c
 *       fs/open.c
 *       fs/read_write.c
 *
 * Phase 7 实现：
 *   - alloc_file()：分配 file 对象
 *   - fput()：释放 file 引用
 *   - alloc_fd()：分配文件描述符号
 *   - fd_install()：将 file 安装到 fd 表中
 *   - fget()：通过 fd 获取 file 对象
 *   - do_sys_open()：内核级 open 实现
 *   - do_sys_close()：内核级 close 实现
 *   - vfs_read()：VFS 读操作
 *   - vfs_write()：VFS 写操作
 *
 * 简化说明：
 *   - file 使用静态池（MAX_FILES = 64）
 *   - fd 表固定大小（NR_OPEN_DEFAULT = 16）
 *   - 无 dup/dup2 支持
 */

#include <linux/types.h>
#include <linux/list.h>
#include <linux/fs.h>

/* 外部函数 */
void boot_printk(const char *s);
void boot_printk_hex(unsigned long val);

/* 路径解析 */
struct dentry *path_lookup(const char *pathname);
struct dentry *path_lookup_create(const char *pathname, struct dentry **parent_out,
                                  struct qstr *last_out);

/* 错误码 */
#define EBADF       9
#define ENOENT      2
#define ENOMEM      12
#define EMFILE      24      /* 打开文件过多 */
#define EEXIST      17
#define EISDIR      21
#define ENOSPC      28
#define EINVAL      22
#define ENOSYS      38

/*
 * ============================================================
 * file 静态池
 * ============================================================
 */
static struct file file_pool[MAX_FILES];
static int file_pool_idx = 0;

/*
 * ============================================================
 * files_init - 初始化 file 子系统
 *
 * 参考：fs/file.c files_init()
 * ============================================================
 */
void files_init(void)
{
    int i;

    for (i = 0; i < MAX_FILES; i++) {
        file_pool[i].f_dentry = NULL;
        file_pool[i].f_inode = NULL;
        file_pool[i].f_op = NULL;
        file_pool[i].f_flags = 0;
        file_pool[i].f_pos = 0;
        file_pool[i].f_count = 0;
    }

    boot_printk("[vfs] file pool initialized (");
    boot_printk_hex(MAX_FILES);
    boot_printk(" entries)\n");
}

/*
 * ============================================================
 * alloc_file - 从静态池分配 file 对象
 *
 * 返回 file 指针（引用计数 = 1），池满返回 NULL。
 *
 * 参考：fs/file_table.c alloc_empty_file()
 * ============================================================
 */
struct file *alloc_file(void)
{
    struct file *filp;

    if (file_pool_idx >= MAX_FILES) {
        boot_printk("[vfs] ERROR: file pool exhausted\n");
        return NULL;
    }

    filp = &file_pool[file_pool_idx++];

    filp->f_dentry = NULL;
    filp->f_inode = NULL;
    filp->f_op = NULL;
    filp->f_flags = 0;
    filp->f_pos = 0;
    filp->f_count = 1;

    return filp;
}

/*
 * ============================================================
 * fput - 释放 file 引用
 *
 * 引用计数减 1，归零时调用 release 回调并回收。
 *
 * @filp: 要释放的 file 对象
 *
 * 参考：fs/file_table.c fput()
 * ============================================================
 */
void fput(struct file *filp)
{
    if (!filp)
        return;

    filp->f_count--;

    if (filp->f_count <= 0) {
        /* 调用文件系统的 release 回调 */
        if (filp->f_op && filp->f_op->release && filp->f_inode)
            filp->f_op->release(filp->f_inode, filp);

        /*
         * 释放 dentry 引用。
         * 注意：不直接调用 iput()。
         * inode 的生命周期由 dentry 管理 — 只要 dentry 存在于 dcache 中，
         * inode 就应保持有效。iput() 仅在 dentry 真正释放时由 dentry 处理。
         *
         * 参考：fs/file_table.c __fput()
         */
        if (filp->f_dentry)
            dput(filp->f_dentry);

        /* 清空 file 对象（Phase 7 不回收到池中）*/
        filp->f_dentry = NULL;
        filp->f_inode = NULL;
        filp->f_op = NULL;
    }
}

/*
 * ============================================================
 * alloc_fd - 分配文件描述符号
 *
 * 从 files_struct 中找到最小的空闲 fd 号。
 *
 * @files: 进程的文件描述符表
 *
 * 返回 fd 号（>= 0），-EMFILE 表示 fd 表已满。
 *
 * 参考：fs/file.c alloc_fd()
 * ============================================================
 */
int alloc_fd(struct files_struct *files)
{
    int fd;

    for (fd = 0; fd < NR_OPEN_DEFAULT; fd++) {
        if (files->fd_array[fd] == NULL)
            return fd;
    }

    return -(int)EMFILE;
}

/*
 * ============================================================
 * fd_install - 将 file 安装到 fd 表
 *
 * @files: 进程的文件描述符表
 * @fd:    文件描述符号
 * @filp:  file 对象
 *
 * 参考：fs/file.c fd_install()
 * ============================================================
 */
void fd_install(struct files_struct *files, int fd, struct file *filp)
{
    if (fd >= 0 && fd < NR_OPEN_DEFAULT)
        files->fd_array[fd] = filp;
}

/*
 * ============================================================
 * fget - 通过 fd 获取 file 对象
 *
 * @files: 进程的文件描述符表
 * @fd:    文件描述符号
 *
 * 返回 file 指针（不增加引用计数），无效 fd 返回 NULL。
 *
 * 参考：fs/file.c fget()
 * ============================================================
 */
struct file *fget(struct files_struct *files, int fd)
{
    if (fd < 0 || fd >= NR_OPEN_DEFAULT)
        return NULL;
    return files->fd_array[fd];
}

/*
 * ============================================================
 * do_sys_open - 内核级 open 实现
 *
 * 打开（或创建）文件，返回文件描述符。
 *
 * @files:     进程的文件描述符表
 * @pathname:  文件路径
 * @flags:     打开标志（O_RDONLY, O_WRONLY, O_CREAT 等）
 * @mode:      创建权限（仅 O_CREAT 时有效）
 *
 * 流程：
 *   1. 分配 fd
 *   2. 路径解析查找 dentry
 *   3. 如果 O_CREAT 且不存在，创建新文件
 *   4. 分配 file 对象
 *   5. 安装到 fd 表
 *
 * 返回 fd（>= 0），负数表示错误码。
 *
 * 参考：fs/open.c do_sys_open() → do_filp_open()
 * ============================================================
 */
int do_sys_open(struct files_struct *files, const char *pathname,
                int flags, unsigned int mode)
{
    struct dentry *dentry;
    struct dentry *parent;
    struct qstr last;
    struct inode *dir_inode;
    struct file *filp;
    int fd;
    int ret;

    /* 1. 分配 fd */
    fd = alloc_fd(files);
    if (fd < 0)
        return fd;

    /* 2. 尝试查找已有的 dentry */
    dentry = path_lookup(pathname);

    if (dentry && dentry->d_inode) {
        /* 文件已存在 */
        if (flags & O_EXCL) {
            /* O_CREAT | O_EXCL：文件必须不存在 */
            dput(dentry);
            return -(int)EEXIST;
        }

        /* 不能 open 一个目录进行写入 */
        if (S_ISDIR(dentry->d_inode->i_mode) && (flags & O_ACCMODE) != O_RDONLY) {
            dput(dentry);
            return -(int)EISDIR;
        }

        /* O_TRUNC：截断文件 */
        if ((flags & O_TRUNC) && S_ISREG(dentry->d_inode->i_mode))
            dentry->d_inode->i_size = 0;

    } else {
        /* 文件不存在 */
        if (dentry)
            dput(dentry);

        if (!(flags & O_CREAT))
            return -(int)ENOENT;

        /* 3. O_CREAT：创建新文件 */
        dentry = path_lookup_create(pathname, &parent, &last);
        if (!dentry) {
            /* path_lookup_create 已返回新建的待填充 dentry */
            return -(int)ENOENT;
        }

        /* 找到父目录 inode，调用 create */
        dir_inode = parent->d_inode;
        if (!dir_inode || !dir_inode->i_op || !dir_inode->i_op->create) {
            dput(dentry);
            return -(int)ENOSYS;
        }

        ret = dir_inode->i_op->create(dir_inode, dentry, mode | S_IFREG, false);
        if (ret != 0) {
            dput(dentry);
            return ret;
        }
    }

    /* 4. 分配 file 对象 */
    filp = alloc_file();
    if (!filp) {
        dput(dentry);
        return -(int)ENOMEM;
    }

    filp->f_dentry = dentry;
    filp->f_inode = dentry->d_inode;
    filp->f_op = dentry->d_inode->i_fop;
    filp->f_flags = (unsigned int)flags;
    filp->f_pos = 0;

    /* O_APPEND：初始位置设为文件末尾 */
    if (flags & O_APPEND)
        filp->f_pos = dentry->d_inode->i_size;

    /* 调用文件系统的 open 回调（如果有）*/
    if (filp->f_op && filp->f_op->open) {
        ret = filp->f_op->open(dentry->d_inode, filp);
        if (ret != 0) {
            fput(filp);
            return ret;
        }
    }

    /* 5. 安装到 fd 表 */
    fd_install(files, fd, filp);

    return fd;
}

/*
 * ============================================================
 * do_sys_close - 内核级 close 实现
 *
 * 关闭文件描述符，释放 file 对象。
 *
 * @files: 进程的文件描述符表
 * @fd:    要关闭的文件描述符
 *
 * 返回 0 成功，负数表示错误。
 *
 * 参考：fs/open.c sys_close()
 * ============================================================
 */
int do_sys_close(struct files_struct *files, int fd)
{
    struct file *filp;

    filp = fget(files, fd);
    if (!filp)
        return -(int)EBADF;

    /* 从 fd 表移除 */
    files->fd_array[fd] = NULL;

    /* 释放 file 对象 */
    fput(filp);

    return 0;
}

/*
 * ============================================================
 * do_sys_unlink - 内核级 unlink 实现
 *
 * 删除文件。调用父目录 inode 的 unlink 回调。
 *
 * @files:    进程的文件描述符表（未使用，保持接口一致性）
 * @pathname: 文件路径
 *
 * 返回 0 成功，负数表示错误。
 *
 * 参考：fs/namei.c do_unlinkat()
 * ============================================================
 */
int do_sys_unlink(struct files_struct *files, const char *pathname)
{
    struct dentry *dentry;
    struct inode *dir_inode;

    (void)files;

    /* 查找目标 dentry */
    dentry = path_lookup(pathname);
    if (!dentry || !dentry->d_inode)
        return -(int)ENOENT;

    /* 不能删除目录（需要 rmdir） */
    if (S_ISDIR(dentry->d_inode->i_mode)) {
        dput(dentry);
        return -(int)EISDIR;
    }

    /* 获取父目录 inode */
    if (!dentry->d_parent || !dentry->d_parent->d_inode) {
        dput(dentry);
        return -(int)ENOENT;
    }
    dir_inode = dentry->d_parent->d_inode;

    /* 调用文件系统的 unlink 回调 */
    if (!dir_inode->i_op || !dir_inode->i_op->unlink) {
        dput(dentry);
        return -(int)ENOSYS;
    }

    return dir_inode->i_op->unlink(dir_inode, dentry);
}

/*
 * ============================================================
 * vfs_read - VFS 读操作
 *
 * 从 file 对象读取数据。调用文件系统的 read 回调。
 *
 * @filp:  file 对象
 * @buf:   用户缓冲区
 * @count: 读取字节数
 *
 * 返回实际读取的字节数，负数表示错误。
 *
 * 参考：fs/read_write.c vfs_read()
 * ============================================================
 */
ssize_t vfs_read(struct file *filp, char *buf, size_t count)
{
    if (!filp || !filp->f_op || !filp->f_op->read)
        return -(ssize_t)EBADF;

    return filp->f_op->read(filp, buf, count, &filp->f_pos);
}

/*
 * ============================================================
 * vfs_write - VFS 写操作
 *
 * 向 file 对象写入数据。调用文件系统的 write 回调。
 *
 * @filp:  file 对象
 * @buf:   数据缓冲区
 * @count: 写入字节数
 *
 * 返回实际写入的字节数，负数表示错误。
 *
 * 参考：fs/read_write.c vfs_write()
 * ============================================================
 */
ssize_t vfs_write(struct file *filp, const char *buf, size_t count)
{
    if (!filp || !filp->f_op || !filp->f_op->write)
        return -(ssize_t)EBADF;

    return filp->f_op->write(filp, buf, count, &filp->f_pos);
}
