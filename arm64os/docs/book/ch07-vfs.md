# Phase 7：VFS 四大对象 + dentry 缓存

## 知识来源总览

- **Linux VFS 接口定义**：约 30%（四大对象字段、操作集接口）
- **UNIX 设计哲学**：约 30%（inode 与文件名分离、fd 语义、一切皆文件）
- **性能优化经验**：约 20%（dcache hash 表、负 dentry、LRU、fd_array 内嵌）
- **POSIX 标准**：约 10%
- **前序 Phase 依赖**：约 10%

## VFS 四大对象

Linux VFS 通过四个对象抽象所有文件系统：

| 对象 | 代表什么 | 生命周期 |
|------|---------|---------|
| super_block | 一个已挂载的文件系统实例 | mount → umount |
| inode | 一个文件的元数据（不含文件名）| 首次访问 → 最后关闭 |
| dentry | 一个路径分量（文件名→inode 映射）| 路径查找时创建，LRU 回收 |
| file | 一个被打开的文件会话 | open → close |

## super_block

```c
struct super_block {
    struct list_head    s_list;         /* 全局超级块链表 */
    dev_t               s_dev;          /* 块设备号 */
    unsigned long       s_blocksize;    /* 块大小 */
    unsigned long       s_magic;        /* 文件系统魔数 */
    struct dentry      *s_root;         /* 根目录 dentry */
    struct file_system_type *s_type;    /* 文件系统类型 */
    const struct super_operations *s_op;/* 操作集（虚函数表）*/
    struct list_head    s_inodes;       /* 所有 inode 链表 */
    void               *s_fs_info;     /* 私有数据 */
};
```

`s_op` 是 C 语言的"虚函数表"模式——每种文件系统提供自己的 `alloc_inode`、`write_inode` 等实现，VFS 通过 `sb->s_op->alloc_inode(sb)` 调用。

## inode

```c
struct inode {
    umode_t             i_mode;     /* 类型 + 权限 */
    unsigned long       i_ino;      /* inode 号 */
    loff_t              i_size;     /* 文件大小 */
    const struct inode_operations *i_op;  /* 目录结构操作 */
    const struct file_operations  *i_fop; /* 文件内容操作 */
    struct super_block *i_sb;
    atomic_t            i_count;    /* 引用计数 */
};
```

**为什么 i_op 和 i_fop 分开？**
- `i_op`：对 inode 本身的操作（lookup、create、mkdir、unlink）—— 改变目录结构
- `i_fop`：对文件内容的操作（read、write、llseek、mmap）—— 改变文件数据

## dentry 与 dcache

```c
struct dentry {
    struct dentry      *d_parent;   /* 父目录 */
    struct qstr         d_name;     /* 文件名（含预计算hash）*/
    struct inode       *d_inode;    /* 关联 inode（NULL=负dentry）*/
    struct list_head    d_lru;      /* LRU 回收链表 */
    struct list_head    d_subdirs;  /* 子目录链表 */
    struct hlist_bl_node d_hash;    /* 全局 hash 表节点 */
    atomic_t            d_count;    /* 引用计数 */
};
```

**负 dentry（d_inode = NULL）**：缓存"文件不存在"的事实。编译器查找头文件时搜索多个目录，大部分查找都失败——负 dentry 避免重复读磁盘。

### d_lookup 四层过滤

```c
struct dentry *d_lookup(const struct dentry *parent,
                        const struct qstr *name) {
    unsigned int hash = full_name_hash(parent, name->name, name->len);
    struct hlist_bl_head *b = dentry_hashtable +
                              (hash & (DENTRY_HASHTABLE_SIZE - 1));

    hlist_bl_for_each_entry_rcu(dentry, b, d_hash) {
        if (dentry->d_parent == parent &&     /* O(1) 指针比较 */
            dentry->d_name.hash == hash &&    /* O(1) hash比较 */
            dentry->d_name.len == name->len && /* O(1) 长度比较 */
            memcmp(...) == 0) {               /* O(n) 字符比较 */
            return dget(dentry);
        }
    }
    return NULL;
}
```

从快到慢逐步排除：parent → hash → 长度 → 字符串。实际到达 memcmp 的频率很低。

## file 与 fd 表

```c
struct file {
    struct path         f_path;       /* dentry + vfsmount */
    struct inode       *f_inode;
    const struct file_operations *f_op;
    loff_t              f_pos;        /* 当前读写偏移 */
    unsigned int        f_flags;      /* O_RDONLY 等 */
};

struct files_struct {
    struct file    *fd_array[NR_OPEN_DEFAULT];  /* 内嵌小型数组 */
};
```

**fd_array 内嵌优化**：大多数进程打开 < 64 个文件，内嵌数组避免额外内存分配。这是 Small Buffer Optimization 模式。

## 路径名解析

```c
static int walk_component(struct nameidata *nd, int flags) {
    dentry = lookup_fast(nd);       /* 查 dcache */
    if (!dentry)
        dentry = lookup_slow(nd);   /* 缓存未命中，读磁盘 */

    if (d_is_symlink(dentry) && !(flags & WALK_NOFOLLOW))
        return follow_link(nd, dentry);

    nd->path.dentry = dentry;
    return 0;
}
```

dcache 命中率通常 > 99%——绝大多数路径查找都在内存中完成。
