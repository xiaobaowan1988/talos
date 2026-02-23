/* SPDX-License-Identifier: GPL-2.0 */
/*
 * arm64os/fs/xfs/xfs_alloc.c
 *
 * XFS B+树空间分配
 *
 * 参考：fs/xfs/xfs_alloc.c
 *       fs/xfs/libxfs/xfs_alloc_btree.c
 *
 * Phase 8 教学简化版：
 *   - 简化 B+树为单层叶节点数组（内存中）
 *   - 每个 AG 维护独立的空闲空间记录
 *   - First-fit 分配策略
 */

#include <linux/types.h>
#include <linux/fs.h>
#include <linux/xfs_format.h>

/* 外部函数 */
void boot_printk(const char *s);
void boot_printk_hex(unsigned long val);
int virtio_blk_read(u64 sector, void *buf, u32 len);
int virtio_blk_write(u64 sector, const void *buf, u32 len);
int xfs_log_write_record(struct xfs_mount *mp, u32 type,
                           const void *data, u32 data_len);

/*
 * ============================================================
 * 内存中的 AG 空闲空间管理
 *
 * 每个 AG 最多 XFS_BTREE_MAX_RECS 个空闲区记录。
 * 记录存储为 CPU 字节序（内存操作），写磁盘时转换为大端。
 * ============================================================
 */
struct ag_free_space {
    u32 numrecs;
    struct xfs_alloc_rec recs[XFS_BTREE_MAX_RECS];
};

static struct ag_free_space ag_free[XFS_AG_COUNT];

/*
 * ============================================================
 * xfs_alloc_init_ag - 初始化 AG 的空闲空间
 *
 * 在格式化或挂载时调用，设置初始空闲区。
 *
 * @mp:          XFS 挂载信息
 * @agno:        AG 编号
 * @first_free:  第一个可用块号（跳过 AG 头部）
 * @ag_length:   AG 总块数
 *
 * 参考：fs/xfs/xfs_alloc.c xfs_alloc_ag_vextent()
 * ============================================================
 */
void xfs_alloc_init_ag(struct xfs_mount *mp, u32 agno,
                        u32 first_free, u32 ag_length)
{
    (void)mp;

    if (agno >= XFS_AG_COUNT)
        return;

    /* 设置单个大空闲区 */
    ag_free[agno].numrecs = 1;
    ag_free[agno].recs[0].ar_startblock = first_free;
    ag_free[agno].recs[0].ar_blockcount = ag_length - first_free;

    boot_printk("[xfs] AG ");
    boot_printk_hex(agno);
    boot_printk(" free space: start=");
    boot_printk_hex(first_free);
    boot_printk(" count=");
    boot_printk_hex(ag_length - first_free);
    boot_printk("\n");
}

/*
 * ============================================================
 * xfs_alloc_block - 从 AG 分配一个块
 *
 * 使用 first-fit 策略：找到第一个够大的空闲区。
 *
 * @mp:      XFS 挂载信息
 * @agno:    AG 编号
 * @bno_out: 输出分配的块号（AG 相对）
 *
 * 返回 0 成功，-1 失败。
 *
 * 参考：fs/xfs/xfs_alloc.c xfs_alloc_ag_vextent_exact()
 * ============================================================
 */
int xfs_alloc_block(struct xfs_mount *mp, u32 agno, u32 *bno_out)
{
    struct ag_free_space *afs;
    u32 i;

    if (agno >= XFS_AG_COUNT)
        return -1;

    afs = &ag_free[agno];

    for (i = 0; i < afs->numrecs; i++) {
        if (afs->recs[i].ar_blockcount > 0) {
            /* 分配第一个块 */
            *bno_out = afs->recs[i].ar_startblock;

            /* 缩减空闲区 */
            afs->recs[i].ar_startblock++;
            afs->recs[i].ar_blockcount--;

            /* 如果空闲区耗尽，移除 */
            if (afs->recs[i].ar_blockcount == 0) {
                u32 j;
                for (j = i; j + 1 < afs->numrecs; j++)
                    afs->recs[j] = afs->recs[j + 1];
                afs->numrecs--;
            }

            /* 更新全局空闲块计数 */
            if (mp->m_fdblocks > 0)
                mp->m_fdblocks--;

            /* 写日志 */
            {
                u32 log_data[2];
                log_data[0] = agno;
                log_data[1] = *bno_out;
                xfs_log_write_record(mp, XFS_LOG_ALLOC,
                                      log_data, sizeof(log_data));
            }

            return 0;
        }
    }

    boot_printk("[xfs] ERROR: AG ");
    boot_printk_hex(agno);
    boot_printk(" out of space\n");
    return -1;
}

/*
 * ============================================================
 * xfs_free_block - 释放一个块回 AG
 *
 * 简化版：将块作为新的单块空闲区添加（不合并相邻）。
 *
 * @mp:   XFS 挂载信息
 * @agno: AG 编号
 * @bno:  块号（AG 相对）
 *
 * 参考：fs/xfs/xfs_alloc.c xfs_free_ag_extent()
 * ============================================================
 */
void xfs_free_block(struct xfs_mount *mp, u32 agno, u32 bno)
{
    struct ag_free_space *afs;

    if (agno >= XFS_AG_COUNT)
        return;

    afs = &ag_free[agno];

    if (afs->numrecs < XFS_BTREE_MAX_RECS) {
        afs->recs[afs->numrecs].ar_startblock = bno;
        afs->recs[afs->numrecs].ar_blockcount = 1;
        afs->numrecs++;
        mp->m_fdblocks++;
    }
}
