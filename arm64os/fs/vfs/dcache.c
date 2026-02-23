/* SPDX-License-Identifier: GPL-2.0 */
/*
 * arm64os/fs/vfs/dcache.c
 *
 * dentry 缓存（dcache）核心
 *
 * 参考：fs/dcache.c
 *
 * Phase 7 实现：
 *   - d_alloc()：分配 dentry 并关联到父目录
 *   - d_alloc_root()：分配根 dentry（无父目录）
 *   - d_lookup()：在 dcache hash 表中查找 dentry
 *   - d_add()：将 dentry 加入 hash 表并关联 inode
 *   - d_instantiate()：将 inode 关联到已有 dentry
 *   - full_name_hash()：计算文件名 hash（dcache 键）
 *   - dcache_init()：初始化 hash 表
 *
 * dcache 是 VFS 的性能关键路径：
 *   open("/etc/passwd") 首次需要逐级查找磁盘，之后命中 dcache 直接返回 dentry。
 *   hash 表以 (parent_dentry, name_hash) 为键，实现 O(1) 查找。
 *
 * 简化说明：
 *   - 使用 list_head（双向链表）作为 hash 桶，而非 hlist（Linux 真实实现）
 *   - 256 个 hash 桶（DCACHE_HASH_SIZE）
 *   - 静态 dentry 池（MAX_DENTRIES = 256）
 *   - 无 LRU 回收（Phase 7 不需要内存压力处理）
 */

#include <linux/types.h>
#include <linux/list.h>
#include <linux/fs.h>

/* 外部函数 */
void boot_printk(const char *s);
void boot_printk_hex(unsigned long val);

/*
 * ============================================================
 * dcache hash 表
 *
 * 使用 list_head 数组，每个桶是一个双向链表。
 * dentry 通过 d_hash 字段挂入对应桶。
 *
 * hash 函数：full_name_hash(parent, name, len)
 * 桶索引 = hash & (DCACHE_HASH_SIZE - 1)
 *
 * 参考：fs/dcache.c dentry_hashtable
 * ============================================================
 */
#define DCACHE_HASH_BITS    8
#define DCACHE_HASH_SIZE    (1 << DCACHE_HASH_BITS)     /* 256 桶 */

static struct list_head dentry_hashtable[DCACHE_HASH_SIZE];

/* dentry 静态池 */
static struct dentry dentry_pool[MAX_DENTRIES];
static int dentry_pool_idx = 0;

/*
 * ============================================================
 * dcache_init - 初始化 dcache hash 表
 *
 * 将所有 hash 桶初始化为空链表。
 *
 * 参考：fs/dcache.c dcache_init()
 * ============================================================
 */
void dcache_init(void)
{
    int i;

    for (i = 0; i < DCACHE_HASH_SIZE; i++)
        INIT_LIST_HEAD(&dentry_hashtable[i]);

    boot_printk("[vfs] dcache initialized (");
    boot_printk_hex(DCACHE_HASH_SIZE);
    boot_printk(" buckets)\n");
}

/*
 * ============================================================
 * full_name_hash - 计算文件名 hash 值
 *
 * 将父 dentry 指针作为 salt，与文件名混合计算 hash。
 * 使用 DJB2 变种算法（简单、分布均匀）。
 *
 * @salt: 通常为父 dentry 指针（区分不同目录下的同名文件）
 * @name: 文件名字符串
 * @len:  文件名长度
 *
 * 返回 32-bit hash 值。
 *
 * 参考：fs/namei.c full_name_hash()（实际使用 partial_name_hash）
 * ============================================================
 */
unsigned int full_name_hash(const void *salt, const char *name, unsigned int len)
{
    unsigned int hash = 5381;
    unsigned int i;

    /* 混入 salt（父 dentry 地址） */
    hash = hash * 33 + (unsigned int)((unsigned long)salt & 0xFFFFFFFF);
    hash = hash * 33 + (unsigned int)(((unsigned long)salt >> 32) & 0xFFFFFFFF);

    /* 混入文件名每个字符 */
    for (i = 0; i < len; i++)
        hash = hash * 33 + (unsigned int)(unsigned char)name[i];

    return hash;
}

/*
 * ============================================================
 * str_copy - 内部字符串复制
 * ============================================================
 */
static void str_copy(char *dst, const char *src, unsigned int len)
{
    unsigned int i;
    for (i = 0; i < len; i++)
        dst[i] = src[i];
    dst[len] = '\0';
}

/*
 * ============================================================
 * str_equal - 字符串比较
 * ============================================================
 */
static int str_equal(const char *a, const char *b, unsigned int len)
{
    unsigned int i;
    for (i = 0; i < len; i++) {
        if (a[i] != b[i])
            return 0;
    }
    return 1;
}

/*
 * ============================================================
 * d_alloc - 分配 dentry 并关联到父目录
 *
 * 从静态池分配 dentry，设置名称和父子关系。
 * 新 dentry 的 d_inode = NULL（负 dentry），
 * 调用者需后续调用 d_add() 或 d_instantiate() 关联 inode。
 *
 * @parent: 父目录 dentry（NULL 表示根 dentry）
 * @name:   文件名 qstr（含 hash）
 *
 * 返回 dentry 指针，池满返回 NULL。
 *
 * 参考：fs/dcache.c d_alloc()
 * ============================================================
 */
struct dentry *d_alloc(struct dentry *parent, const struct qstr *name)
{
    struct dentry *dentry;

    if (dentry_pool_idx >= MAX_DENTRIES) {
        boot_printk("[vfs] ERROR: dentry pool exhausted\n");
        return NULL;
    }

    dentry = &dentry_pool[dentry_pool_idx++];

    /* 复制文件名到内嵌缓冲区 */
    if (name->len < sizeof(dentry->d_iname)) {
        str_copy(dentry->d_iname, name->name, name->len);
        dentry->d_name.name = dentry->d_iname;
    } else {
        /* 文件名超过内嵌缓冲区大小 — Phase 7 不处理长文件名 */
        boot_printk("[vfs] ERROR: filename too long\n");
        dentry_pool_idx--;
        return NULL;
    }
    dentry->d_name.len = name->len;
    dentry->d_name.hash = name->hash;

    /* 设置父目录关系 */
    dentry->d_parent = parent ? parent : dentry; /* 根 dentry 的 parent 是自身 */
    dentry->d_inode = NULL;
    dentry->d_sb = parent ? parent->d_sb : NULL;
    dentry->d_op = NULL;
    dentry->d_count = 1;

    /* 初始化链表头 */
    INIT_LIST_HEAD(&dentry->d_child);
    INIT_LIST_HEAD(&dentry->d_subdirs);
    INIT_LIST_HEAD(&dentry->d_hash);

    /* 加入父目录的子目录链表 */
    if (parent)
        list_add_tail(&dentry->d_child, &parent->d_subdirs);

    return dentry;
}

/*
 * ============================================================
 * d_alloc_root - 分配根 dentry
 *
 * 为文件系统根目录创建 dentry（名称 "/"，无父目录）。
 *
 * @sb: 超级块
 *
 * 返回根 dentry 指针。
 *
 * 参考：fs/dcache.c d_make_root()
 * ============================================================
 */
struct dentry *d_alloc_root(struct super_block *sb)
{
    struct qstr root_name;
    struct dentry *root;

    root_name.name = "/";
    root_name.len = 1;
    root_name.hash = full_name_hash(NULL, "/", 1);

    root = d_alloc(NULL, &root_name);
    if (root) {
        root->d_sb = sb;
        root->d_parent = root;  /* 根的 parent 是自身 */
    }

    return root;
}

/*
 * ============================================================
 * d_add - 将 dentry 加入 hash 表并关联 inode
 *
 * 1. 将 inode 关联到 dentry（d_instantiate）
 * 2. 将 dentry 插入 dcache hash 表
 *
 * 之后 d_lookup() 可以找到此 dentry。
 *
 * @dentry: 要添加的 dentry
 * @inode:  关联的 inode
 *
 * 参考：fs/dcache.c d_add()
 * ============================================================
 */
void d_add(struct dentry *dentry, struct inode *inode)
{
    unsigned int bucket;

    /* 关联 inode */
    dentry->d_inode = inode;

    /* 计算 hash 桶索引 */
    bucket = dentry->d_name.hash & (DCACHE_HASH_SIZE - 1);

    /* 插入 hash 表 */
    list_add(&dentry->d_hash, &dentry_hashtable[bucket]);
}

/*
 * ============================================================
 * d_instantiate - 将 inode 关联到已有 dentry
 *
 * 与 d_add 不同，不插入 hash 表（dentry 已在 hash 表中）。
 *
 * @dentry: 目标 dentry
 * @inode:  要关联的 inode
 *
 * 参考：fs/dcache.c d_instantiate()
 * ============================================================
 */
void d_instantiate(struct dentry *dentry, struct inode *inode)
{
    dentry->d_inode = inode;
}

/*
 * ============================================================
 * d_lookup - 在 dcache 中查找 dentry
 *
 * 通过 (parent, name) 在 hash 表中查找已缓存的 dentry。
 * 这是 VFS 路径查找的快速路径（fast path）。
 *
 * 查找算法：
 *   1. hash = full_name_hash(parent, name, len)
 *   2. bucket = hash & (DCACHE_HASH_SIZE - 1)
 *   3. 遍历桶中的 dentry 链表，匹配 parent + name + len
 *
 * @parent: 父目录 dentry
 * @name:   要查找的文件名 qstr
 *
 * 返回匹配的 dentry（引用计数 +1），未命中返回 NULL。
 *
 * 参考：fs/dcache.c d_lookup() → __d_lookup()
 * ============================================================
 */
struct dentry *d_lookup(const struct dentry *parent, const struct qstr *name)
{
    unsigned int hash;
    unsigned int bucket;
    struct list_head *pos;

    /* 计算 hash */
    hash = full_name_hash(parent, name->name, name->len);
    bucket = hash & (DCACHE_HASH_SIZE - 1);

    /* 遍历 hash 桶 */
    list_for_each(pos, &dentry_hashtable[bucket]) {
        struct dentry *dentry = list_entry(pos, struct dentry, d_hash);

        /* 匹配条件：parent 相同 + hash 相同 + 名称长度相同 + 名称相同 */
        if (dentry->d_parent == parent &&
            dentry->d_name.hash == hash &&
            dentry->d_name.len == name->len &&
            str_equal(dentry->d_name.name, name->name, name->len)) {
            /* Cache hit! 增加引用计数并返回 */
            dentry->d_count++;
            return dentry;
        }
    }

    /* Cache miss */
    return NULL;
}

/*
 * ============================================================
 * dget / dput - dentry 引用计数管理
 *
 * 参考：include/linux/dcache.h dget() / fs/dcache.c dput()
 * ============================================================
 */
struct dentry *dget(struct dentry *dentry)
{
    if (dentry)
        dentry->d_count++;
    return dentry;
}

void dput(struct dentry *dentry)
{
    if (!dentry)
        return;

    if (dentry->d_count > 0)
        dentry->d_count--;

    /*
     * 引用计数归零时：不从 hash 表移除。
     * 这是 dcache 的核心特性 — 未使用的 dentry 保留在缓存中，
     * 后续路径查找仍可命中（避免重复的文件系统 lookup）。
     * 仅在内存压力下才会回收（Phase 7 不实现 LRU 回收）。
     *
     * 参考：fs/dcache.c dput() — 归零后加入 LRU 链表而非删除
     */
}
