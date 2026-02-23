/* SPDX-License-Identifier: GPL-2.0 */
/*
 * arm64os/fs/vfs/inode.c
 *
 * inode 管理与缓存
 *
 * 参考：fs/inode.c
 *
 * Phase 7 实现：
 *   - new_inode()：分配并初始化新 inode
 *   - iget()：增加 inode 引用计数
 *   - iput()：释放 inode 引用（计数归零时回收）
 *   - inode_init()：初始化 inode 池
 *
 * 简化说明：
 *   - 使用静态池（MAX_INODES = 128），无 slab 分配器
 *   - 无 inode hash 缓存（按 ino 查找）— Phase 7 通过 dentry 访问 inode
 *   - 引用计数使用普通 int（单 CPU，无竞争）
 */

#include <linux/types.h>
#include <linux/list.h>
#include <linux/fs.h>

/* 外部函数 */
void boot_printk(const char *s);
void boot_printk_hex(unsigned long val);

/*
 * ============================================================
 * inode 静态池
 * ============================================================
 */
static struct inode inode_pool[MAX_INODES];
static int inode_pool_idx = 0;

/*
 * ============================================================
 * inode_init - 初始化 inode 子系统
 *
 * 清零 inode 池。在 vfs_init() 中调用。
 *
 * 参考：fs/inode.c inode_init()
 * ============================================================
 */
void inode_init(void)
{
    int i;

    for (i = 0; i < MAX_INODES; i++) {
        inode_pool[i].i_ino = 0;
        inode_pool[i].i_mode = 0;
        inode_pool[i].i_size = 0;
        inode_pool[i].i_sb = NULL;
        inode_pool[i].i_op = NULL;
        inode_pool[i].i_fop = NULL;
        inode_pool[i].i_count = 0;
        inode_pool[i].i_private = NULL;
        INIT_LIST_HEAD(&inode_pool[i].i_sb_list);
    }

    boot_printk("[vfs] inode pool initialized (");
    boot_printk_hex(MAX_INODES);
    boot_printk(" entries)\n");
}

/*
 * ============================================================
 * new_inode - 分配新 inode
 *
 * 从静态池分配一个 inode，分配唯一的 i_ino，
 * 加入超级块的 inode 链表。
 *
 * @sb: 所属超级块
 *
 * 返回 inode 指针，池满返回 NULL。
 *
 * 参考：fs/inode.c new_inode()
 * ============================================================
 */
struct inode *new_inode(struct super_block *sb)
{
    struct inode *inode;

    if (inode_pool_idx >= MAX_INODES) {
        boot_printk("[vfs] ERROR: inode pool exhausted\n");
        return NULL;
    }

    inode = &inode_pool[inode_pool_idx++];

    /* 初始化基础字段 */
    inode->i_ino = sb->s_next_ino++;
    inode->i_mode = 0;
    inode->i_size = 0;
    inode->i_sb = sb;
    inode->i_op = NULL;
    inode->i_fop = NULL;
    inode->i_count = 1;         /* 初始引用计数 = 1 */
    inode->i_private = NULL;

    /* 加入超级块的 inode 链表 */
    INIT_LIST_HEAD(&inode->i_sb_list);
    list_add_tail(&inode->i_sb_list, &sb->s_inodes);

    return inode;
}

/*
 * ============================================================
 * iget - 增加 inode 引用计数
 *
 * @inode: 要引用的 inode
 *
 * 返回 inode 本身（便于链式调用）。
 *
 * 参考：fs/inode.c iget5_locked()（简化版）
 * ============================================================
 */
struct inode *iget(struct inode *inode)
{
    if (inode)
        inode->i_count++;
    return inode;
}

/*
 * ============================================================
 * iput - 释放 inode 引用
 *
 * 引用计数减 1。计数归零时：
 *   - 如果超级块提供 destroy_inode，调用之
 *   - 否则只是标记为可回收（Phase 7 简化版不实际回收）
 *
 * @inode: 要释放的 inode
 *
 * 参考：fs/inode.c iput()
 * ============================================================
 */
void iput(struct inode *inode)
{
    if (!inode)
        return;

    inode->i_count--;

    if (inode->i_count <= 0) {
        /* 从超级块链表移除 */
        list_del(&inode->i_sb_list);

        /* 调用文件系统的 destroy_inode（如果有）*/
        if (inode->i_sb && inode->i_sb->s_op &&
            inode->i_sb->s_op->destroy_inode)
            inode->i_sb->s_op->destroy_inode(inode);

        /* Phase 7 简化：不回收到池中（静态分配）*/
        inode->i_ino = 0;
        inode->i_sb = NULL;
    }
}
