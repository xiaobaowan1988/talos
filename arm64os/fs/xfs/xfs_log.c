/* SPDX-License-Identifier: GPL-2.0 */
/*
 * arm64os/fs/xfs/xfs_log.c
 *
 * XFS WAL 日志核心
 *
 * 参考：fs/xfs/xfs_log.c
 *       fs/xfs/xfs_log_recover.c
 *
 * Phase 8 教学简化版：
 *   - 循环日志缓冲区（fixed-size ring buffer）
 *   - 日志记录使用大端序
 *   - 支持写入、提交、恢复
 */

#include <linux/types.h>
#include <linux/fs.h>
#include <linux/xfs_format.h>

/* 外部函数 */
void boot_printk(const char *s);
void boot_printk_hex(unsigned long val);
int virtio_blk_read(u64 sector, void *buf, u32 len);
int virtio_blk_write(u64 sector, const void *buf, u32 len);

/*
 * ============================================================
 * 内部辅助
 * ============================================================
 */
static void log_mem_zero(void *dst, u32 len)
{
    u8 *p = (u8 *)dst;
    u32 i;
    for (i = 0; i < len; i++)
        p[i] = 0;
}

static void log_mem_copy(void *dst, const void *src, u32 len)
{
    u8 *d = (u8 *)dst;
    const u8 *s = (const u8 *)src;
    u32 i;
    for (i = 0; i < len; i++)
        d[i] = s[i];
}

/*
 * ============================================================
 * xfs_log_init - 初始化日志子系统
 *
 * 设置日志 LSN 和写入头位置。
 *
 * @mp: XFS 挂载信息
 *
 * 参考：fs/xfs/xfs_log.c xlog_alloc_log()
 * ============================================================
 */
void xfs_log_init(struct xfs_mount *mp)
{
    mp->m_log_lsn = 1;
    mp->m_log_head = 0;

    boot_printk("[xfs] Log initialized: start=");
    boot_printk_hex((unsigned long)mp->m_logstart);
    boot_printk(" blocks=");
    boot_printk_hex(mp->m_logblocks);
    boot_printk("\n");
}

/*
 * ============================================================
 * xfs_log_write_record - 写入一条日志记录
 *
 * 将操作记录序列化为大端序并写入循环日志。
 *
 * @mp:       XFS 挂载信息
 * @type:     日志项类型（XFS_LOG_INODE_CREATE 等）
 * @data:     附带数据（可为 NULL）
 * @data_len: 数据长度
 *
 * 返回 0 成功，-1 失败。
 *
 * 参考：fs/xfs/xfs_log.c xlog_write()
 * ============================================================
 */
int xfs_log_write_record(struct xfs_mount *mp, u32 type,
                           const void *data, u32 data_len)
{
    static u8 __attribute__((aligned(4096))) log_buf[4096];
    struct xfs_log_record *rec;
    u64 log_sector;
    u32 rec_len;

    /* 计算记录长度 */
    rec_len = sizeof(struct xfs_log_record) + data_len;

    /* 清零缓冲区 */
    log_mem_zero(log_buf, 512);

    /* 构建日志记录（大端序） */
    rec = (struct xfs_log_record *)log_buf;
    rec->h_magicno = cpu_to_be32(XFS_LOG_MAGIC);
    rec->h_len = cpu_to_be32(rec_len);
    rec->h_lsn = cpu_to_be64(mp->m_log_lsn);
    rec->h_type = cpu_to_be32(type);
    rec->h_num_logops = cpu_to_be32(1);

    /* 附带数据 */
    if (data && data_len > 0 && data_len <= 512 - sizeof(struct xfs_log_record))
        log_mem_copy(log_buf + sizeof(struct xfs_log_record), data, data_len);

    /*
     * 计算日志扇区地址：
     * 日志起始块 * (block_size / 512) + 日志头位置 * (block_size / 512)
     *
     * 简化：每个日志记录占用一个块（4KB = 8 扇区）
     */
    log_sector = XFS_PART_START +
                 (mp->m_logstart + mp->m_log_head) * (XFS_BLOCK_SIZE / 512);

    if (virtio_blk_write(log_sector, log_buf, 512) != 0) {
        boot_printk("[xfs] ERROR: log write failed\n");
        return -1;
    }

    /* 推进日志头和 LSN */
    mp->m_log_head = (mp->m_log_head + 1) % mp->m_logblocks;
    mp->m_log_lsn++;

    return 0;
}

/*
 * ============================================================
 * xfs_log_commit - 写入事务提交标记
 *
 * 在日志中写入 COMMIT 记录，标志事务完成。
 *
 * @mp: XFS 挂载信息
 *
 * 返回 0 成功。
 *
 * 参考：fs/xfs/xfs_log.c xlog_commit_record()
 * ============================================================
 */
int xfs_log_commit(struct xfs_mount *mp)
{
    return xfs_log_write_record(mp, XFS_LOG_COMMIT, NULL, 0);
}

/*
 * ============================================================
 * xfs_log_recover - 崩溃恢复：扫描并重放日志
 *
 * 扫描日志区域，查找有效记录。
 * 教学简化版：仅扫描并报告，不实际重放。
 *
 * @mp: XFS 挂载信息
 *
 * 返回找到的有效日志记录数。
 *
 * 参考：fs/xfs/xfs_log_recover.c xlog_do_recover()
 * ============================================================
 */
int xfs_log_recover(struct xfs_mount *mp)
{
    static u8 __attribute__((aligned(4096))) rec_buf[512];
    struct xfs_log_record *rec;
    u32 i;
    int count = 0;

    boot_printk("[xfs] Log recovery: scanning ");
    boot_printk_hex(mp->m_logblocks);
    boot_printk(" log blocks...\n");

    for (i = 0; i < mp->m_logblocks; i++) {
        u64 sector = XFS_PART_START +
                     (mp->m_logstart + i) * (XFS_BLOCK_SIZE / 512);

        if (virtio_blk_read(sector, rec_buf, 512) != 0)
            continue;

        rec = (struct xfs_log_record *)rec_buf;

        if (be32_to_cpu(rec->h_magicno) == XFS_LOG_MAGIC) {
            count++;
        }
    }

    boot_printk("[xfs] Log recovery: found ");
    boot_printk_hex((unsigned long)count);
    boot_printk(" valid records\n");

    return count;
}
