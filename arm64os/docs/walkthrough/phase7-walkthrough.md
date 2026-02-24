# Phase 7 Walkthrough: VFS 四大对象 + dentry 缓存

> **目标**：实现 Linux VFS 抽象层，让不同文件系统共享统一的 open/read/write/close 接口。
> **最终效果**：创建文件、写入、读取、dcache 命中、子目录操作全部通过。

---

## 7.1 VFS 的设计哲学

Linux 的所有文件系统（ext4, XFS, tmpfs, procfs...）共享同一套用户接口：`open()`, `read()`, `write()`, `close()`。这靠 VFS（Virtual File System）实现。

VFS 定义**四大核心对象**和对应的**操作表**：

```
  用户程序
    open("/test.txt", O_RDONLY)
        │
        ▼
  ┌─────────────────────────────────────────┐
  │              VFS 抽象层                  │
  │                                         │
  │  super_block  ←─→  super_operations     │
  │  inode        ←─→  inode_operations     │
  │  dentry       ←─→  (dcache 缓存)       │
  │  file         ←─→  file_operations      │
  │                                         │
  └────────────────┬────────────────────────┘
                   │ 调用具体文件系统的操作
        ┌──────────┼──────────┐
        ▼          ▼          ▼
      ramfs      squashfs    XFS
```

---

## 7.2 四大对象详解

### super_block — 文件系统实例

```c
struct super_block {
    unsigned long s_dev;         /* 设备号 */
    unsigned long s_blocksize;   /* 块大小 */
    unsigned long s_magic;       /* 文件系统魔数 */
    struct dentry *s_root;       /* 根目录 dentry */
    struct super_operations *s_op; /* 操作表 */
    void *s_fs_info;             /* 文件系统私有数据 */
};
```

**一个 super_block = 一次挂载**。挂载 ramfs 到 `/` 创建一个，挂载 squashfs 到 `/sq` 再创建一个。

### inode — 文件元数据

```c
struct inode {
    unsigned long i_ino;         /* inode 编号 */
    unsigned int i_mode;         /* 类型 + 权限 (S_IFREG, 0644) */
    unsigned int i_uid, i_gid;   /* 所有者 */
    loff_t i_size;               /* 文件大小 */
    struct inode_operations *i_op;  /* inode 操作（lookup, create, mkdir） */
    struct file_operations *i_fop;  /* 默认文件操作（read, write） */
    struct super_block *i_sb;    /* 所属超级块 */
    unsigned int i_count;        /* 引用计数 */
    void *i_private;             /* 文件系统私有数据 */
};
```

**inode = 磁盘上的文件**，不包含文件名（文件名在 dentry 中）。

### dentry — 目录项缓存

```c
struct dentry {
    struct qstr d_name;          /* 文件名 {hash, len, name} */
    struct inode *d_inode;       /* 关联的 inode（NULL = negative dentry） */
    struct dentry *d_parent;     /* 父目录 */
    struct list_head d_child;    /* 在父目录的子节点链表中 */
    struct list_head d_subdirs;  /* 子节点链表头 */
    unsigned long d_hash;        /* 哈希值（dcache 查找用） */
    int d_count;                 /* 引用计数 */
};
```

**dentry = 路径中的一个组件**。路径 `/home/user/file.txt` 产生 4 个 dentry：
`/` → `home` → `user` → `file.txt`。

### file — 打开的文件

```c
struct file {
    struct dentry *f_path;       /* 关联的 dentry */
    struct inode *f_inode;       /* 关联的 inode */
    struct file_operations *f_op; /* 文件操作表 */
    loff_t f_pos;                /* 当前读写偏移 */
    unsigned int f_flags;        /* 打开标志 (O_RDONLY, ...) */
    int f_count;                 /* 引用计数 */
};
```

**file = 进程打开的一个文件描述**。同一个 inode 可以被多个进程打开，每个进程有独立的 `f_pos`。

---

## 7.3 dcache — 路径解析加速器

### 原理

每次 `open("/path/to/file")` 都要解析路径。没有缓存的话需要逐级读磁盘。dcache 用哈希表缓存 dentry，实现 O(1) 路径查找。

### 哈希函数

```c
unsigned int full_name_hash(const struct dentry *parent,
                            const char *name, unsigned int len)
{
    unsigned int hash = 5381;  /* DJB2 初始值 */
    hash = hash * 33 + (unsigned long)parent;  /* 混入父指针 */
    for (unsigned int i = 0; i < len; i++)
        hash = hash * 33 + name[i];
    return hash;
}
```

**关键**：哈希值包含**父 dentry 指针**，所以 `/a/test` 和 `/b/test` 的 `test` dentry 哈希不同。

### dcache 查找

```c
#define DCACHE_HASH_BITS    8
#define DCACHE_HASH_SIZE    (1 << DCACHE_HASH_BITS)  /* 256 桶 */

static struct list_head dcache_hashtable[DCACHE_HASH_SIZE];

struct dentry *d_lookup(const struct dentry *parent, const struct qstr *name)
{
    unsigned int hash = full_name_hash(parent, name->name, name->len);
    struct list_head *bucket = &dcache_hashtable[hash & (DCACHE_HASH_SIZE - 1)];

    /* 遍历桶中的 dentry */
    list_for_each_entry(dentry, bucket, d_hash_node) {
        if (dentry->d_parent == parent &&
            dentry->d_name.len == name->len &&
            memcmp(dentry->d_name.name, name->name, name->len) == 0)
            return dentry;  /* 命中！ */
    }
    return NULL;  /* 未命中 → 需要文件系统 lookup */
}
```

---

## 7.4 路径解析 — `path_lookup()`

```c
struct dentry *path_lookup(const char *pathname)
{
    /* Step 1: 检查挂载点 */
    struct mount_entry *mnt = find_mount(pathname);
    struct dentry *dentry = mnt ? mnt->mnt_root : root_dentry;

    /* Step 2: 跳过挂载点前缀 */
    const char *remaining = pathname + mount_prefix_len;

    /* Step 3: 逐组件解析 */
    while (*remaining) {
        /* 提取下一个组件名 */
        char component[NAME_MAX];
        int len = extract_component(remaining, component);

        /* 快速路径: dcache 查找 */
        struct qstr name = { .name = component, .len = len };
        name.hash = full_name_hash(dentry, component, len);

        struct dentry *child = d_lookup(dentry, &name);

        if (!child) {
            /* 慢速路径: 调用文件系统的 lookup */
            child = d_alloc(dentry, &name);
            dentry->d_inode->i_op->lookup(dentry->d_inode, child, 0);
        }

        dentry = child;
        remaining += len;
    }

    return dentry;
}
```

```
  path_lookup("/subdir/hello.txt")

  "/" ──dcache──► dentry(/)
                    │
  "subdir" ──dcache──► dentry(subdir) ← hit!
                          │
  "hello.txt" ──dcache──► dentry(hello.txt) ← miss!
                          │
                  调用 ramfs_lookup()
                          │
                  创建 dentry, d_add 到 dcache
```

---

## 7.5 文件操作实现

### do_sys_open — 打开文件

```c
int do_sys_open(struct files_struct *files, const char *pathname,
                int flags, unsigned int mode)
{
    /* Step 1: 分配文件描述符 */
    int fd = alloc_fd(files);

    /* Step 2: 路径解析 */
    struct dentry *dentry;
    if (flags & O_CREAT) {
        /* 查找父目录 + 获取文件名 */
        struct dentry *parent;
        const char *filename;
        parent = path_lookup_create(pathname, &filename);

        /* 检查文件是否已存在 */
        dentry = d_lookup(parent, &name);
        if (!dentry) {
            dentry = d_alloc(parent, &name);
            /* 调用文件系统的 create */
            parent->d_inode->i_op->create(parent->d_inode,
                                          dentry, mode);
        }
    } else {
        dentry = path_lookup(pathname);
    }

    /* Step 3: 创建 file 对象 */
    struct file *filp = alloc_file();
    filp->f_path = dentry;
    filp->f_inode = dentry->d_inode;
    filp->f_op = dentry->d_inode->i_fop;
    filp->f_pos = 0;
    filp->f_flags = flags;

    /* Step 4: 安装到 fd 表 */
    fd_install(files, fd, filp);

    return fd;
}
```

### vfs_read / vfs_write — 读写

```c
ssize_t vfs_read(struct file *filp, char *buf, size_t count)
{
    if (!filp->f_op || !filp->f_op->read)
        return -1;

    /* 委托给具体文件系统的 read 操作 */
    ssize_t ret = filp->f_op->read(filp, buf, count, &filp->f_pos);

    return ret;
}
```

---

## 7.6 ramfs — 最简单的文件系统

ramfs 是内存文件系统，数据直接存在 RAM 中，不涉及磁盘。

```c
/* 每个文件的私有数据 */
struct ramfs_inode_info {
    void *data;      /* 数据页地址 */
    int used;
};

/* 创建文件 */
static int ramfs_create(struct inode *dir, struct dentry *dentry,
                        unsigned int mode)
{
    struct inode *inode = new_inode(dir->i_sb);
    inode->i_mode = S_IFREG | mode;
    inode->i_size = 0;
    inode->i_op = &ramfs_file_inode_ops;
    inode->i_fop = &ramfs_file_ops;

    /* 分配私有数据（延迟到首次写入时分配数据页） */
    struct ramfs_inode_info *info = alloc_ramfs_info();
    info->data = NULL;
    inode->i_private = info;

    /* 连接 dentry ↔ inode */
    d_add(dentry, inode);
    return 0;
}

/* 写入 */
static ssize_t ramfs_write(struct file *filp, const char *buf,
                           size_t count, loff_t *pos)
{
    struct ramfs_inode_info *info = filp->f_inode->i_private;

    /* 首次写入：分配数据页 */
    if (!info->data) {
        struct page *page = alloc_pages(0);  /* 4KB */
        info->data = page_address(page);
    }

    /* 拷贝数据 */
    memcpy(info->data + *pos, buf, count);
    *pos += count;
    if (*pos > filp->f_inode->i_size)
        filp->f_inode->i_size = *pos;

    return count;
}
```

---

## 7.7 文件系统注册与挂载

```c
/* 注册文件系统类型 */
static struct file_system_type ramfs_type = {
    .name = "ramfs",
    .mount = ramfs_mount,
};

void ramfs_init(void)
{
    register_filesystem(&ramfs_type);
}

/* 挂载 */
int do_mount(const char *dev, const char *path,
             const char *fstype, unsigned long flags, void *data)
{
    /* 查找文件系统类型 */
    struct file_system_type *type = get_fs_type(fstype);

    /* 调用文件系统的 mount 回调 */
    struct super_block *sb = type->mount(type, flags, dev, data);

    /* 记录挂载点 */
    mount_table[mount_count].mnt_path = path;
    mount_table[mount_count].mnt_root = sb->s_root;
    mount_table[mount_count].mnt_sb = sb;
    mount_count++;

    return 0;
}
```

---

## 7.8 静态资源池

Phase 7 使用静态内存池代替动态分配（简化实现）：

```c
static struct super_block super_pool[4];         /* 最多 4 个文件系统 */
static struct inode inode_pool[128];             /* 最多 128 个 inode */
static struct dentry dentry_pool[256];           /* 最多 256 个 dentry */
static struct file file_pool[64];                /* 最多 64 个打开的文件 */
static struct mount_entry mount_table[8];        /* 最多 8 个挂载点 */
```

---

## 7.9 测试验证

```c
static void test_vfs(void)
{
    /* Test 1: 创建并写入 /test.txt */
    int fd = do_sys_open(&init_files, "/test.txt", O_CREAT|O_WRONLY, 0644);
    struct file *filp = fget(&init_files, fd);
    vfs_write(filp, "hello vfs\n", 10);
    do_sys_close(&init_files, fd);

    /* Test 2: 重新打开并读取 */
    fd = do_sys_open(&init_files, "/test.txt", O_RDONLY, 0);
    filp = fget(&init_files, fd);
    char buf[32];
    ssize_t n = vfs_read(filp, buf, 16);
    /* 验证 n == 10 且内容 == "hello vfs\n" */

    /* Test 3: dcache 命中 — 再次打开同一文件 */
    int fd2 = do_sys_open(&init_files, "/test.txt", O_RDONLY, 0);
    /* d_lookup 直接命中，不需要调用 ramfs_lookup */

    /* Test 4: 子目录 */
    /* mkdir /subdir → 创建 /subdir/hello.txt → 读回验证 */
}
```

---

## 7.10 Phase 7 核心概念总结

| 概念 | 说明 |
|------|------|
| **super_block** | 文件系统实例，每次挂载一个 |
| **inode** | 文件元数据（大小、权限、操作表），不含文件名 |
| **dentry** | 路径组件缓存，连接名字和 inode |
| **file** | 进程打开的文件描述（位置、标志） |
| **dcache** | 哈希表缓存 dentry，O(1) 路径查找 |
| **操作表** | 函数指针结构体，实现多态（不同 FS 不同实现） |
| **ramfs** | 最简内存文件系统，数据存 RAM |

**Phase 7 奠定的基础**：VFS 框架可以挂载任何文件系统。Phase 8 的 squashfs/XFS 只需实现操作表中的回调函数。
