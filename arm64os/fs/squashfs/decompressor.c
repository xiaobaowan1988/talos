/* SPDX-License-Identifier: GPL-2.0 */
/*
 * arm64os/fs/squashfs/decompressor.c
 *
 * squashfs 解压器接口
 *
 * 参考：fs/squashfs/decompressor.c
 *       fs/squashfs/lz4_wrapper.c
 *
 * Phase 8 教学简化版：
 *   - 仅支持 SQUASHFS_COMP_NONE（未压缩，直接拷贝）
 *   - LZ4 解压为简化 stub（教学环境无完整 LZ4 库）
 *   - 足够演示解压器分发机制
 */

#include <linux/types.h>
#include <linux/squashfs_fs.h>

/* 外部函数 */
void boot_printk(const char *s);

/*
 * ============================================================
 * mem_copy - 内存复制
 * ============================================================
 */
static void mem_copy(void *dst, const void *src, u32 len)
{
    u8 *d = (u8 *)dst;
    const u8 *s = (const u8 *)src;
    u32 i;

    for (i = 0; i < len; i++)
        d[i] = s[i];
}

/*
 * ============================================================
 * squashfs_decompress - 解压数据块
 *
 * @compression: 压缩算法 ID
 * @src:         压缩数据源
 * @src_len:     源数据长度
 * @dst:         解压目标缓冲区
 * @dst_len:     目标缓冲区大小
 *
 * 返回解压后的数据长度，失败返回 -1。
 *
 * 参考：fs/squashfs/decompressor.c squashfs_decompress()
 * ============================================================
 */
int squashfs_decompress(u16 compression, const void *src, u32 src_len,
                         void *dst, u32 dst_len)
{
    switch (compression) {
    case SQUASHFS_COMP_NONE:
        /* 未压缩：直接拷贝 */
        if (src_len > dst_len)
            src_len = dst_len;
        mem_copy(dst, src, src_len);
        return (int)src_len;

    case SQUASHFS_COMP_LZ4:
        /*
         * LZ4 解压 stub。
         * 教学环境中测试镜像使用未压缩数据，
         * 此处仅作为接口示意。
         */
        boot_printk("[squashfs] WARN: LZ4 decompression not implemented, "
                     "using raw copy\n");
        if (src_len > dst_len)
            src_len = dst_len;
        mem_copy(dst, src, src_len);
        return (int)src_len;

    default:
        boot_printk("[squashfs] ERROR: unsupported compression\n");
        return -1;
    }
}
