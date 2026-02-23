/* SPDX-License-Identifier: GPL-2.0 */
/*
 * arm64os/include/linux/squashfs_fs.h
 *
 * squashfs 磁盘格式定义
 *
 * 参考：include/linux/squashfs_fs.h
 *       include/uapi/linux/squashfs_fs.h
 *
 * Phase 8 教学简化版：
 *   - 小端序（ARM64 原生字节序，无需转换）
 *   - 固定大小 inode（48字节，无变长 block list）
 *   - 支持未压缩 + LZ4 压缩
 *   - inode/directory table 存储为未压缩
 */

#ifndef __LINUX_SQUASHFS_FS_H
#define __LINUX_SQUASHFS_FS_H

#include <linux/types.h>

/*
 * ============================================================
 * 魔数与常量
 * ============================================================
 */
#define SQUASHFS_MAGIC          0x73717368  /* "sqsh" 小端序 */
#define SQUASHFS_MAJOR          4
#define SQUASHFS_MINOR          0

/* 压缩算法 ID */
#define SQUASHFS_COMP_NONE      0   /* 未压缩（教学扩展）*/
#define SQUASHFS_COMP_ZLIB      1
#define SQUASHFS_COMP_LZ4       5
#define SQUASHFS_COMP_ZSTD      6

/* 块压缩标志：块大小最高位为 1 表示该块未压缩 */
#define SQUASHFS_COMPRESSED_BIT_BLOCK  0x01000000

/* inode 类型 */
#define SQUASHFS_DIR_TYPE       1   /* 基本目录 */
#define SQUASHFS_REG_TYPE       2   /* 基本普通文件 */

/* 分区参数 */
#define SQFS_PART_START         2048    /* squashfs 分区起始扇区 */
#define SQFS_PART_SECTORS       4096    /* squashfs 分区扇区数（2MB）*/

/* 默认块大小 */
#define SQUASHFS_DEFAULT_BLOCK_SIZE     4096
#define SQUASHFS_DEFAULT_BLOCK_LOG      12

/*
 * ============================================================
 * squashfs 超级块（96字节）
 *
 * 位于分区偏移 0。小端序。
 *
 * 参考：include/linux/squashfs_fs.h struct squashfs_super_block
 * ============================================================
 */
struct squashfs_super_block {
    u32 s_magic;                /* 0x73717368 ("sqsh") */
    u32 inodes;                 /* inode 总数 */
    u32 mkfs_time;              /* 创建时间戳 */
    u32 block_size;             /* 数据块大小（默认 4096）*/
    u32 fragments;              /* 碎片块数量 */
    u16 compression;            /* 压缩算法 ID */
    u16 block_log;              /* log2(block_size) */
    u16 flags;                  /* 标志位 */
    u16 no_ids;                 /* UID/GID 表项数 */
    u16 s_major;                /* 版本主号（4）*/
    u16 s_minor;                /* 版本次号（0）*/
    u64 root_inode;             /* 根 inode 在 inode table 中的字节偏移 */
    u64 bytes_used;             /* 文件系统已使用总字节数 */
    u64 id_table_start;         /* （简化版未使用）*/
    u64 xattr_id_table_start;   /* （简化版未使用）*/
    u64 inode_table_start;      /* inode table 的字节偏移（相对分区起始）*/
    u64 directory_table_start;  /* directory table 的字节偏移 */
    u64 fragment_table_start;   /* （简化版未使用）*/
    u64 lookup_table_start;     /* （简化版未使用）*/
};

/*
 * ============================================================
 * squashfs inode（简化固定大小，48字节）
 *
 * 教学简化：不使用 Linux 内核的变长 inode（basic/extended），
 * 而是统一使用固定大小结构体，便于理解。
 *
 * 参考：
 *   include/linux/squashfs_fs.h struct squashfs_base_inode_header
 *   include/linux/squashfs_fs.h struct squashfs_reg_inode_header
 *   include/linux/squashfs_fs.h struct squashfs_dir_inode_header
 * ============================================================
 */
struct squashfs_inode {
    u16 inode_type;             /* SQUASHFS_DIR_TYPE 或 SQUASHFS_REG_TYPE */
    u16 mode;                   /* 权限位（如 0755）*/
    u32 inode_number;           /* inode 编号（从 1 开始）*/
    u32 file_size;              /* 文件大小（字节）*/
    u32 parent_inode;           /* 父目录 inode 号（0 表示根）*/
    u64 start_block;            /* 文件数据起始字节偏移（相对分区）*/
    u32 block_count;            /* 数据块数量 */
    u32 dir_offset;             /* 目录项在 dir table 中的字节偏移 */
    u32 dir_size;               /* 目录项总大小（字节）*/
    u32 _pad[3];                /* 对齐到 48 字节 */
};

/*
 * ============================================================
 * squashfs 目录项（变长）
 *
 * 在 directory table 中连续存放。
 * 每个目录项: 8字节头 + name_size字节文件名
 *
 * 参考：include/linux/squashfs_fs.h struct squashfs_dir_entry
 * ============================================================
 */
struct squashfs_dir_entry {
    u32 inode_number;           /* 对应 inode 编号 */
    u16 inode_type;             /* 类型（SQUASHFS_DIR_TYPE 等）*/
    u16 name_size;              /* 文件名长度（不含 '\0'）*/
    /* char name[]; — 紧随其后 */
};

/*
 * ============================================================
 * squashfs 内存中的 inode 私有数据
 *
 * 挂接到 VFS inode 的 i_private 字段。
 * ============================================================
 */
struct squashfs_inode_info {
    u32 block_size;             /* 数据块大小 */
    u64 start_block;            /* 数据起始偏移（相对分区）*/
    u32 block_count;            /* 数据块数 */
    u32 dir_offset;             /* 目录项在 dir table 中的偏移 */
    u32 dir_size;               /* 目录项总大小 */
    u16 compression;            /* 压缩算法 */
    u64 inode_table_start;      /* inode table 起始偏移 */
    u64 dir_table_start;        /* directory table 起始偏移 */
};

/*
 * ============================================================
 * squashfs 超级块私有数据
 *
 * 挂接到 VFS super_block 的 s_fs_info 字段。
 * ============================================================
 */
struct squashfs_sb_info {
    u32 block_size;
    u32 block_log;
    u16 compression;
    u32 inodes;
    u64 inode_table_start;      /* inode table 磁盘字节偏移 */
    u64 dir_table_start;        /* directory table 磁盘字节偏移 */
    u64 root_inode_offset;      /* 根 inode 在 inode table 中的偏移 */
};

/*
 * ============================================================
 * 函数声明
 * ============================================================
 */

/* super.c */
void squashfs_init(void);
void squashfs_mkfs_test(void);

/* inode.c */
struct inode *squashfs_iget(struct super_block *sb, u32 inode_number);
int squashfs_read_inode(struct super_block *sb, struct squashfs_inode *raw,
                        u32 inode_number);

/* dir.c */
struct dentry *squashfs_lookup(struct inode *dir, struct dentry *dentry,
                                unsigned int flags);

/* file.c */
ssize_t squashfs_read(struct file *filp, char *buf, size_t count,
                       unsigned long *pos);

/* decompressor.c */
int squashfs_decompress(u16 compression, const void *src, u32 src_len,
                         void *dst, u32 dst_len);

#endif /* __LINUX_SQUASHFS_FS_H */
