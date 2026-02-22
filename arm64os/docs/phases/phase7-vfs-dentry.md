# Phase 7：VFS四大对象 + dentry缓存

## 参考内核文件

```
fs/super.c              # 超级块（superblock）操作
fs/inode.c              # inode管理、inode缓存
fs/dcache.c             # dentry缓存（dcache）核心
fs/file.c               # 文件对象、文件描述符表
fs/namei.c              # 路径名解析（path_lookup）
fs/read_write.c         # read/write系统调用实现
fs/open.c               # open/close系统调用
include/linux/fs.h      # VFS四大对象数据结构定义
```

---

## 7.1 VFS四大核心对象

Linux VFS（Virtual File System）通过四个对象抽象所有文件系统：

```
                    open("/etc/passwd", O_RDONLY)
                              │
                    路径解析 (namei.c)
                              │
              ┌───────────────┼───────────────┐
              ▼               ▼               ▼
         superblock         inode           dentry
       (文件系统实例)    (文件元数据)    (目录项缓存)
              │               │
              └───────────────┘
                      │
                    file
                  (进程视角的
                   打开文件)
```

## 7.2 superblock（超级块）

```c
/* 参考 include/linux/fs.h */
struct super_block {
    struct list_head    s_list;         /* 所有超级块链表 */
    dev_t               s_dev;          /* 块设备号 */
    unsigned long       s_blocksize;    /* 块大小（字节）*/
    unsigned long       s_magic;        /* 文件系统魔数 */
    struct dentry      *s_root;         /* 根目录 dentry */
    struct file_system_type *s_type;    /* 文件系统类型 */
    const struct super_operations *s_op; /* 超级块操作集 */
    struct list_head    s_inodes;       /* 所有 inode 链表 */
    void               *s_fs_info;      /* 文件系统私有数据 */
};

/* 超级块操作集 */
struct super_operations {
    struct inode *(*alloc_inode)(struct super_block *sb);
    void          (*destroy_inode)(struct inode *);
    void          (*dirty_inode)(struct inode *, int flags);
    int           (*write_inode)(struct inode *, struct writeback_control *);
    void          (*put_super)(struct super_block *);
    int           (*statfs)(struct dentry *, struct kstatfs *);
};
```

## 7.3 inode（索引节点）

```c
/* 参考 include/linux/fs.h */
struct inode {
    umode_t             i_mode;     /* 文件类型 + 权限（如 S_IFREG|0644）*/
    unsigned long       i_ino;      /* inode号 */
    kuid_t              i_uid;
    kgid_t              i_gid;
    loff_t              i_size;     /* 文件大小（字节）*/
    struct timespec64   i_atime;    /* 访问时间 */
    struct timespec64   i_mtime;    /* 修改时间 */
    struct timespec64   i_ctime;    /* 状态改变时间 */
    unsigned long       i_blkbits;  /* 块大小的位数 */
    blkcnt_t            i_blocks;   /* 占用的512字节块数 */
    const struct inode_operations *i_op;  /* inode操作集 */
    const struct file_operations  *i_fop; /* 文件操作集 */
    struct super_block *i_sb;
    struct address_space *i_mapping; /* 页缓存 */
    atomic_t            i_count;    /* 引用计数 */
};

/* inode操作集（目录相关）*/
struct inode_operations {
    struct dentry *(*lookup)(struct inode *, struct dentry *, unsigned int);
    int            (*create)(struct inode *, struct dentry *, umode_t, bool);
    int            (*mkdir)(struct inode *, struct dentry *, umode_t);
    int            (*unlink)(struct inode *, struct dentry *);
    int            (*rename)(struct inode *, struct dentry *,
                             struct inode *, struct dentry *, unsigned int);
    int            (*getattr)(const struct path *, struct kstat *, u32, unsigned int);
};

/* 文件操作集（文件 I/O）*/
struct file_operations {
    loff_t  (*llseek)(struct file *, loff_t, int);
    ssize_t (*read)(struct file *, char __user *, size_t, loff_t *);
    ssize_t (*write)(struct file *, const char __user *, size_t, loff_t *);
    int     (*open)(struct inode *, struct file *);
    int     (*release)(struct inode *, struct file *);
    int     (*fsync)(struct file *, loff_t, loff_t, int);
    int     (*mmap)(struct file *, struct vm_area_struct *);
};
```

## 7.4 dentry（目录项）与 dcache

```c
/* 参考 include/linux/dcache.h */
struct dentry {
    unsigned int        d_flags;
    struct dentry      *d_parent;   /* 父目录 dentry */
    struct qstr         d_name;     /* 文件名（含hash）*/
    struct inode       *d_inode;    /* 对应的 inode（NULL=负dentry）*/
    struct super_block *d_sb;
    const struct dentry_operations *d_op;

    /* LRU链表（dcache回收用）*/
    struct list_head    d_lru;
    /* 子目录链表 */
    struct list_head    d_child;
    struct list_head    d_subdirs;
    /* hash表节点 */
    struct hlist_bl_node d_hash;
    atomic_t            d_count;    /* 引用计数 */
};

/*
 * dcache 是路径查找的缓存层：
 * 避免每次 open("/a/b/c") 都要读磁盘查找目录项
 *
 * 全局 hash 表：dentry_hashtable[hash(parent, name)] → dentry 链
 * 参考 fs/dcache.c : d_lookup()
 */
struct dentry *d_lookup(const struct dentry *parent, const struct qstr *name) {
    unsigned int hash = full_name_hash(parent, name->name, name->len);
    struct hlist_bl_head *b = dentry_hashtable + (hash & (DENTRY_HASHTABLE_SIZE - 1));

    struct dentry *dentry;
    hlist_bl_for_each_entry_rcu(dentry, b, d_hash) {
        if (dentry->d_parent == parent &&
            dentry->d_name.hash == hash &&
            dentry->d_name.len == name->len &&
            memcmp(dentry->d_name.name, name->name, name->len) == 0) {
            return dget(dentry);  /* 增加引用计数 */
        }
    }
    return NULL;
}
```

## 7.5 file（文件对象）

```c
/* 参考 include/linux/fs.h */
struct file {
    struct path         f_path;       /* 包含 dentry + vfsmount */
    struct inode       *f_inode;
    const struct file_operations *f_op;
    spinlock_t          f_lock;
    atomic_long_t       f_count;
    unsigned int        f_flags;      /* O_RDONLY, O_WRONLY, O_NONBLOCK... */
    fmode_t             f_mode;
    loff_t              f_pos;        /* 当前读写偏移（seek位置）*/
    void               *private_data; /* 文件系统私有数据 */
};

/* 进程的文件描述符表 */
struct files_struct {
    atomic_t        count;
    struct fdtable  *fdt;           /* 指向文件描述符表 */
    struct file    *fd_array[NR_OPEN_DEFAULT]; /* 内嵌的小型fd数组 */
};

/* fd → file 的转换 */
struct file *fget(unsigned int fd) {
    struct files_struct *files = current->files;
    if (fd >= files->fdt->max_fds) return NULL;
    return rcu_dereference(files->fdt->fd[fd]);
}
```

## 7.6 路径名解析（参考 fs/namei.c）

```c
/*
 * open("/etc/passwd", O_RDONLY) 触发的路径解析过程：
 *
 * 1. 从根目录（/）或当前目录（.）的 dentry 开始
 * 2. 逐个处理路径分量（"etc" → "passwd"）
 * 3. 每个分量先查 dcache，命中则直接用，未命中则调用
 *    具体文件系统的 inode->i_op->lookup() 从磁盘读取
 */

/* 参考 fs/namei.c : walk_component() */
static int walk_component(struct nameidata *nd, int flags) {
    struct dentry *dentry;
    struct inode  *inode;

    /* 1. 查 dcache */
    dentry = lookup_fast(nd);
    if (!dentry) {
        /* 2. 缓存未命中，调用文件系统 lookup */
        dentry = lookup_slow(nd);
    }

    /* 3. 处理符号链接 */
    if (d_is_symlink(dentry) && !(flags & WALK_NOFOLLOW))
        return follow_link(nd, dentry);

    nd->path.dentry = dentry;
    nd->inode = dentry->d_inode;
    return 0;
}
```

## 7.7 文件系统注册与挂载

```c
/* 参考 fs/super.c */

/* 注册文件系统类型 */
struct file_system_type {
    const char *name;            /* "ext4", "xfs", "squashfs"... */
    int         fs_flags;
    struct dentry *(*mount)(struct file_system_type *, int,
                            const char *, void *);
    void (*kill_sb)(struct super_block *);
};

/* 挂载流程 */
int do_mount(const char *dev_name, const char *dir_name,
             const char *type, unsigned long flags, void *data) {
    /* 1. 找到文件系统类型 */
    struct file_system_type *fstype = get_fs_type(type);

    /* 2. 调用文件系统的 mount() 读取超级块 */
    struct dentry *root = fstype->mount(fstype, flags, dev_name, data);

    /* 3. 在挂载点 dentry 上安装 vfsmount */
    struct vfsmount *mnt = alloc_vfsmnt(dev_name);
    mnt->mnt_root = root;
    mnt->mnt_sb   = root->d_sb;

    /* 4. 将 mnt 挂到 dir_name 对应的 dentry 上 */
    attach_mnt(mnt, lookup_dentry(dir_name));
    return 0;
}
```

## 7.8 验证方法

```c
void test_vfs(void) {
    /* 挂载 ramfs 到 / */
    do_mount("none", "/", "ramfs", 0, NULL);

    /* 创建文件 */
    int fd = sys_openat(AT_FDCWD, "/test.txt",
                        O_CREAT | O_WRONLY, 0644);
    sys_write(fd, "hello vfs\n", 10);
    sys_close(fd);

    /* 重新读取 */
    fd = sys_openat(AT_FDCWD, "/test.txt", O_RDONLY, 0);
    char buf[16];
    sys_read(fd, buf, 10);
    sys_close(fd);

    printk("VFS test OK: %s\n", buf);
}
```

## 7.9 本阶段产出文件

```
arm64os/
└── fs/
    ├── vfs/
    │   ├── super.c      ← 超级块管理（核心）
    │   ├── inode.c      ← inode管理与缓存
    │   ├── dcache.c     ← dentry缓存（核心）
    │   ├── file.c       ← 文件对象与fd表
    │   └── namei.c      ← 路径名解析
    └── ramfs/
        └── ramfs.c      ← 最简内存文件系统（用于测试）
```
