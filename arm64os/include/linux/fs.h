/* SPDX-License-Identifier: GPL-2.0 */
/*
 * arm64os/include/linux/fs.h
 *
 * VFS (Virtual File System) 四大核心对象与操作集
 *
 * 参考：include/linux/fs.h
 *       include/linux/dcache.h
 *       include/linux/mount.h
 *
 * Phase 7 实现：
 *   - struct super_block：文件系统实例（超级块）
 *   - struct inode：文件元数据（索引节点）
 *   - struct dentry：目录项缓存（dcache）
 *   - struct file：打开的文件对象（进程视角）
 *   - struct files_struct：进程文件描述符表
 *   - struct file_system_type：文件系统类型注册
 *
 * 简化说明：
 *   - 使用静态池分配（无 slab/kmalloc）
 *   - 单 CPU，无自旋锁（无 SMP）
 *   - 无 RCU / seqlock
 *   - 引用计数使用普通 int（无 atomic_t）
 */

#ifndef __LINUX_FS_H
#define __LINUX_FS_H

#include <linux/types.h>
#include <linux/list.h>

/* ---- 前向声明 ---- */
struct super_block;
struct inode;
struct dentry;
struct file;
struct file_system_type;
struct inode_operations;
struct file_operations;
struct super_operations;
struct dentry_operations;

/*
 * ============================================================
 * 文件类型与权限位（参考 include/uapi/linux/stat.h）
 * ============================================================
 */
#define S_IFMT      0170000     /* 文件类型掩码 */
#define S_IFREG     0100000     /* 普通文件 */
#define S_IFDIR     0040000     /* 目录 */
#define S_IFLNK     0120000     /* 符号链接 */

#define S_ISREG(m)  (((m) & S_IFMT) == S_IFREG)
#define S_ISDIR(m)  (((m) & S_IFMT) == S_IFDIR)
#define S_ISLNK(m)  (((m) & S_IFMT) == S_IFLNK)

/* 权限位 */
#define S_IRWXU     00700       /* owner rwx */
#define S_IRUSR     00400       /* owner read */
#define S_IWUSR     00200       /* owner write */
#define S_IXUSR     00100       /* owner execute */
#define S_IRWXG     00070       /* group rwx */
#define S_IRWXO     00007       /* other rwx */

/*
 * ============================================================
 * open(2) 标志位（参考 include/uapi/asm-generic/fcntl.h）
 * ============================================================
 */
#define O_RDONLY    0x0000
#define O_WRONLY    0x0001
#define O_RDWR      0x0002
#define O_ACCMODE   0x0003      /* 访问模式掩码 */
#define O_CREAT     0x0040      /* 不存在则创建 */
#define O_EXCL      0x0080      /* 与 O_CREAT 一起使用，文件必须不存在 */
#define O_TRUNC     0x0200      /* 截断为零长度 */
#define O_APPEND    0x0400      /* 追加模式 */

/*
 * AT_FDCWD - 相对于当前工作目录（参考 include/uapi/linux/fcntl.h）
 */
#define AT_FDCWD    (-100)

/*
 * lseek(2) whence 值
 */
#define SEEK_SET    0           /* 从文件起始位置 */
#define SEEK_CUR    1           /* 从当前位置 */
#define SEEK_END    2           /* 从文件末尾 */

/*
 * ============================================================
 * 文件描述符表参数
 * ============================================================
 */
#define NR_OPEN_DEFAULT     16  /* 默认每进程最大打开文件数 */

/*
 * ============================================================
 * VFS 对象池大小（静态分配）
 * ============================================================
 */
#define MAX_SUPERBLOCKS     4
#define MAX_INODES          128
#define MAX_DENTRIES        256
#define MAX_FILES           64

/*
 * ============================================================
 * qstr - 快速字符串（带预计算 hash）
 *
 * 参考：include/linux/dcache.h struct qstr
 *
 * dentry 中的文件名使用 qstr，预计算 hash 值以加速 dcache 查找。
 * ============================================================
 */
struct qstr {
    unsigned int    hash;       /* 文件名 hash（由 full_name_hash 计算）*/
    unsigned int    len;        /* 文件名长度（不含 '\0'）*/
    const char     *name;       /* 文件名字符串 */
};

/*
 * ============================================================
 * super_block - 文件系统实例（超级块）
 *
 * 每个挂载的文件系统对应一个 super_block。
 * 管理该文件系统的所有 inode，提供文件系统级操作。
 *
 * 参考：include/linux/fs.h struct super_block
 * ============================================================
 */
struct super_block {
    unsigned long               s_magic;        /* 文件系统魔数 */
    unsigned long               s_blocksize;    /* 块大小（字节）*/
    struct dentry              *s_root;         /* 根目录 dentry */
    struct file_system_type    *s_type;         /* 文件系统类型 */
    const struct super_operations *s_op;        /* 超级块操作集 */
    struct list_head            s_inodes;       /* 此 sb 的所有 inode 链表 */
    void                       *s_fs_info;      /* 文件系统私有数据 */
    unsigned long               s_next_ino;     /* 下一个可用 inode 号 */
};

/*
 * super_operations - 超级块操作集
 *
 * 参考：include/linux/fs.h struct super_operations
 */
struct super_operations {
    struct inode *(*alloc_inode)(struct super_block *sb);
    void          (*destroy_inode)(struct inode *);
    void          (*put_super)(struct super_block *);
};

/*
 * ============================================================
 * inode - 文件元数据（索引节点）
 *
 * 每个文件/目录对应一个 inode，存储文件元数据（类型、大小、权限）。
 * 链接到 inode_operations（目录操作）和 file_operations（文件 I/O）。
 *
 * 参考：include/linux/fs.h struct inode
 * ============================================================
 */
struct inode {
    unsigned long               i_ino;          /* inode 号 */
    unsigned int                i_mode;         /* 文件类型 + 权限（如 S_IFREG|0644）*/
    unsigned long               i_size;         /* 文件大小（字节）*/
    struct super_block         *i_sb;           /* 所属超级块 */
    const struct inode_operations  *i_op;       /* inode 操作集 */
    const struct file_operations   *i_fop;      /* 默认文件操作集 */
    int                         i_count;        /* 引用计数 */
    void                       *i_private;      /* 文件系统私有数据 */
    struct list_head            i_sb_list;      /* 超级块 inode 链表节点 */
};

/*
 * inode_operations - inode 操作集（目录相关）
 *
 * 参考：include/linux/fs.h struct inode_operations
 */
struct inode_operations {
    struct dentry *(*lookup)(struct inode *dir, struct dentry *dentry,
                             unsigned int flags);
    int            (*create)(struct inode *dir, struct dentry *dentry,
                             unsigned int mode, bool excl);
    int            (*mkdir)(struct inode *dir, struct dentry *dentry,
                            unsigned int mode);
    int            (*unlink)(struct inode *dir, struct dentry *dentry);
};

/*
 * file_operations - 文件操作集（文件 I/O）
 *
 * 参考：include/linux/fs.h struct file_operations
 */
struct file_operations {
    ssize_t (*read)(struct file *filp, char *buf, size_t count, unsigned long *pos);
    ssize_t (*write)(struct file *filp, const char *buf, size_t count,
                     unsigned long *pos);
    int     (*open)(struct inode *inode, struct file *filp);
    int     (*release)(struct inode *inode, struct file *filp);
};

/*
 * ============================================================
 * dentry - 目录项缓存（dcache）
 *
 * 将路径名分量（如 "etc"、"passwd"）与 inode 关联。
 * 通过全局 hash 表实现 O(1) 路径缓存查找。
 *
 * 参考：include/linux/dcache.h struct dentry
 * ============================================================
 */
struct dentry {
    struct dentry              *d_parent;       /* 父目录 dentry */
    struct qstr                 d_name;         /* 文件名（含 hash）*/
    struct inode               *d_inode;        /* 对应的 inode（NULL=负 dentry）*/
    struct super_block         *d_sb;           /* 所属超级块 */
    const struct dentry_operations *d_op;       /* dentry 操作集 */
    struct list_head            d_child;        /* 在父目录 d_subdirs 链表中的节点 */
    struct list_head            d_subdirs;      /* 子目录/文件链表头 */
    struct list_head            d_hash;         /* dcache hash 桶链表节点 */
    int                         d_count;        /* 引用计数 */
    char                        d_iname[32];    /* 内嵌短文件名存储 */
};

/*
 * dentry_operations - dentry 操作集
 *
 * 参考：include/linux/dcache.h struct dentry_operations
 */
struct dentry_operations {
    int (*d_revalidate)(struct dentry *, unsigned int);
};

/*
 * ============================================================
 * file - 打开的文件对象（进程视角）
 *
 * 每次 open(2) 调用创建一个 file 对象，记录当前读写位置和访问模式。
 * 通过文件描述符（fd）从进程的 files_struct 访问。
 *
 * 参考：include/linux/fs.h struct file
 * ============================================================
 */
struct file {
    struct dentry              *f_dentry;       /* 关联的 dentry */
    struct inode               *f_inode;        /* 关联的 inode（冗余，方便访问）*/
    const struct file_operations *f_op;         /* 文件操作集 */
    unsigned int                f_flags;        /* 打开标志（O_RDONLY 等）*/
    unsigned long               f_pos;          /* 当前读写偏移 */
    int                         f_count;        /* 引用计数 */
};

/*
 * ============================================================
 * files_struct - 进程的文件描述符表
 *
 * 每个进程拥有一个 files_struct，将 fd 号映射到 file 对象。
 * fd_array 为固定大小数组（简化版，无动态扩展）。
 *
 * 参考：include/linux/fdtable.h struct files_struct
 * ============================================================
 */
struct files_struct {
    struct file    *fd_array[NR_OPEN_DEFAULT];  /* fd → file 映射 */
    int             next_fd;                    /* 下一个可分配的 fd 号 */
};

/*
 * ============================================================
 * file_system_type - 文件系统类型注册
 *
 * 每种文件系统（ext4、xfs、ramfs 等）注册一个 file_system_type。
 * mount 时通过名称查找，调用 mount() 回调读取超级块。
 *
 * 参考：include/linux/fs.h struct file_system_type
 * ============================================================
 */
struct file_system_type {
    const char *name;                           /* 文件系统名称 */
    struct dentry *(*mount)(struct file_system_type *fs_type,
                            int flags, const char *dev_name, void *data);
    void (*kill_sb)(struct super_block *sb);
    struct file_system_type *next;              /* 已注册文件系统链表 */
};

/*
 * ============================================================
 * vfsmount - 挂载点信息
 *
 * 记录文件系统的挂载信息。Phase 7 简化版：仅支持根挂载。
 *
 * 参考：include/linux/mount.h struct vfsmount
 * ============================================================
 */
struct vfsmount {
    struct dentry  *mnt_root;                   /* 挂载文件系统的根 dentry */
    struct super_block *mnt_sb;                 /* 挂载文件系统的超级块 */
};

/*
 * ============================================================
 * VFS 子系统接口（由 fs/vfs/ 各源文件提供）
 * ============================================================
 */

/* --- super.c --- */
void vfs_init(void);
int register_filesystem(struct file_system_type *fs);
struct file_system_type *get_fs_type(const char *name);
struct super_block *alloc_super(struct file_system_type *type);
int do_mount(const char *dev_name, const char *dir_name,
             const char *type, unsigned long flags, void *data);

/* --- inode.c --- */
void inode_init(void);
struct inode *new_inode(struct super_block *sb);
void iput(struct inode *inode);
struct inode *iget(struct inode *inode);

/* --- dcache.c --- */
void dcache_init(void);
struct dentry *d_alloc(struct dentry *parent, const struct qstr *name);
struct dentry *d_alloc_root(struct super_block *sb);
struct dentry *d_lookup(const struct dentry *parent, const struct qstr *name);
void d_add(struct dentry *dentry, struct inode *inode);
void d_instantiate(struct dentry *dentry, struct inode *inode);
unsigned int full_name_hash(const void *salt, const char *name, unsigned int len);
struct dentry *dget(struct dentry *dentry);
void dput(struct dentry *dentry);

/* --- file.c --- */
void files_init(void);
struct file *alloc_file(void);
void fput(struct file *filp);
int alloc_fd(struct files_struct *files);
void fd_install(struct files_struct *files, int fd, struct file *filp);
struct file *fget(struct files_struct *files, int fd);

/* --- namei.c --- */
struct dentry *path_lookup(const char *pathname);
struct dentry *path_lookup_create(const char *pathname, struct dentry **parent_out,
                                  struct qstr *last_out);

/* --- 全局根文件系统 --- */
extern struct vfsmount *root_mnt;
extern struct files_struct init_files;

#endif /* __LINUX_FS_H */
