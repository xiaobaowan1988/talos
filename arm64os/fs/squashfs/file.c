/* SPDX-License-Identifier: GPL-2.0 */
/*
 * arm64os/fs/squashfs/file.c
 *
 * squashfs 文件数据读取
 *
 * 参考：fs/squashfs/file.c
 *       fs/squashfs/block.c
 *
 * Phase 8 教学简化版：
 *   - 支持未压缩数据块的读取
 *   - 通过 VirtIO 块设备读取磁盘数据
 *   - 单块读取（不支持多块拼接）
 */

#include <linux/types.h>
#include <linux/fs.h>
#include <linux/squashfs_fs.h>

/* 外部函数 */
void boot_printk(const char *s);
void boot_printk_hex(unsigned long val);
int virtio_blk_read(u64 sector, void *buf, u32 len);
int squashfs_decompress(u16 compression, const void *src, u32 src_len,
                         void *dst, u32 dst_len);

/*
 * ============================================================
 * 内部辅助：内存复制
 * ============================================================
 */
static void sqfs_mem_copy(char *dst, const char *src, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++)
        dst[i] = src[i];
}

/*
 * ============================================================
 * squashfs_read - 读取 squashfs 文件数据
 *
 * @filp:  file 对象
 * @buf:   目标缓冲区
 * @count: 请求读取的字节数
 * @pos:   读写偏移指针
 *
 * 返回实际读取的字节数。
 *
 * 参考：fs/squashfs/file.c squashfs_readpage()
 * ============================================================
 */
ssize_t squashfs_read(struct file *filp, char *buf, size_t count,
                       unsigned long *pos)
{
    struct inode *inode = filp->f_inode;
    struct squashfs_inode_info *si;
    u64 disk_offset;
    u64 sector;
    u32 block_idx;
    u32 block_offset;
    u32 avail;
    /* I/O 缓冲区 */
    static u8 __attribute__((aligned(4096))) read_buf[4096];

    if (!inode || !inode->i_private)
        return 0;

    si = (struct squashfs_inode_info *)inode->i_private;

    /* 检查偏移是否超出文件大小 */
    if (*pos >= inode->i_size)
        return 0;

    avail = (u32)(inode->i_size - *pos);
    if (count > avail)
        count = avail;

    if (count == 0)
        return 0;

    /*
     * 计算该偏移属于哪个数据块
     *
     * block_idx:    块索引
     * block_offset: 块内偏移
     */
    block_idx = (u32)(*pos / si->block_size);
    block_offset = (u32)(*pos % si->block_size);

    /*
     * 从磁盘读取数据
     *
     * 计算数据在磁盘上的精确位置（包含块内偏移），
     * 然后按 512 字节扇区读取。
     *
     * 简化实现：每次读取一个 512 字节扇区，适用于教学场景。
     */
    disk_offset = si->start_block + (u64)block_idx * si->block_size + block_offset;
    sector = SQFS_PART_START + disk_offset / 512;

    /* 限制读取量不超过单块剩余空间 */
    if (count > si->block_size - block_offset)
        count = si->block_size - block_offset;

    /* 读取包含目标数据的扇区 */
    if (virtio_blk_read(sector, read_buf, 512) != 0) {
        boot_printk("[squashfs] ERROR: read data block failed\n");
        return 0;
    }

    /*
     * 如果数据是压缩的，解压；
     * 教学镜像使用未压缩数据，compression = NONE
     */
    {
        u32 sector_off = (u32)(disk_offset % 512);
        u32 avail_in_sector = 512 - sector_off;

        if (count > avail_in_sector)
            count = avail_in_sector;

        if (si->compression != SQUASHFS_COMP_NONE) {
            static u8 __attribute__((aligned(4096))) decomp_buf[512];
            int ret;

            ret = squashfs_decompress(si->compression,
                                       read_buf + sector_off,
                                       (u32)count, decomp_buf,
                                       sizeof(decomp_buf));
            if (ret < 0)
                return 0;

            sqfs_mem_copy(buf, (char *)decomp_buf, count);
        } else {
            sqfs_mem_copy(buf, (char *)read_buf + sector_off, count);
        }
    }

    *pos += count;
    return (ssize_t)count;
}

/*
 * ============================================================
 * 操作集定义
 * ============================================================
 */

/* squashfs 只读 — write 为 NULL */
const struct inode_operations squashfs_file_inode_ops = {
    .lookup = NULL,
    .create = NULL,
    .mkdir  = NULL,
    .unlink = NULL,
};

const struct file_operations squashfs_file_fops = {
    .read    = squashfs_read,
    .write   = NULL,        /* 只读文件系统 */
    .open    = NULL,
    .release = NULL,
};
