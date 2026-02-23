/* SPDX-License-Identifier: GPL-2.0 */
/*
 * arm64os/include/linux/xfs_format.h
 *
 * XFS 磁盘格式定义
 *
 * 参考：fs/xfs/libxfs/xfs_format.h
 *       fs/xfs/libxfs/xfs_types.h
 *
 * Phase 8 教学简化版：
 *   - XFS 使用大端序（big-endian），需要字节序转换
 *   - 简化 AG 结构（2 个 AG）
 *   - 简化 B+树（单层叶节点）
 *   - 简化 inode（固定 256 字节）
 *   - WAL 日志记录关键元数据操作
 */

#ifndef __LINUX_XFS_FORMAT_H
#define __LINUX_XFS_FORMAT_H

#include <linux/types.h>

/*
 * ============================================================
 * 大端序转换宏
 *
 * XFS 的所有磁盘数据结构使用大端序（跨平台一致性）。
 * ARM64 默认小端序，需要字节转换。
 *
 * 参考：include/linux/byteorder/little_endian.h
 * ============================================================
 */
static inline u16 be16_to_cpu(u16 val)
{
    return (u16)((val >> 8) | (val << 8));
}

static inline u32 be32_to_cpu(u32 val)
{
    return ((val & 0x000000FFU) << 24) |
           ((val & 0x0000FF00U) <<  8) |
           ((val & 0x00FF0000U) >>  8) |
           ((val & 0xFF000000U) >> 24);
}

static inline u64 be64_to_cpu(u64 val)
{
    u32 hi = be32_to_cpu((u32)(val & 0xFFFFFFFFUL));
    u32 lo = be32_to_cpu((u32)(val >> 32));
    return ((u64)hi << 32) | (u64)lo;
}

#define cpu_to_be16(v) be16_to_cpu(v)
#define cpu_to_be32(v) be32_to_cpu(v)
#define cpu_to_be64(v) be64_to_cpu(v)

/*
 * ============================================================
 * 魔数与常量
 * ============================================================
 */
#define XFS_SB_MAGIC            0x58465342  /* "XFSB"（大端序值）*/
#define XFS_AGF_MAGIC           0x58414746  /* "XAGF" */
#define XFS_AGI_MAGIC           0x58414749  /* "XAGI" */
#define XFS_ABTB_MAGIC          0x41425442  /* "ABTB" — bnobt */
#define XFS_ABTC_MAGIC          0x41425443  /* "ABTC" — cntbt */
#define XFS_IBT_MAGIC           0x49414254  /* "IABT" — inobt */
#define XFS_LOG_MAGIC           0xFEEDbabe  /* 日志记录魔数 */

/* 分区参数 */
#define XFS_PART_START          6144    /* XFS 分区起始扇区 */
#define XFS_PART_SECTORS        10240   /* XFS 分区扇区数（5MB）*/

/* XFS 文件系统参数 */
#define XFS_BLOCK_SIZE          4096    /* 块大小 */
#define XFS_BLOCK_LOG           12      /* log2(4096) */
#define XFS_SECTOR_SIZE         512
#define XFS_INODE_SIZE          256     /* inode 大小 */
#define XFS_INODE_LOG           8       /* log2(256) */
#define XFS_INODES_PER_BLOCK    (XFS_BLOCK_SIZE / XFS_INODE_SIZE)  /* 16 */

/* AG 参数 */
#define XFS_AG_COUNT            2       /* AG 数量 */

/* 日志参数 */
#define XFS_LOG_BLOCKS          32      /* 日志区大小（块数）*/

/* B+树参数 */
#define XFS_BTREE_MAX_RECS      64      /* 每个叶节点最大记录数 */

/* inode 分配参数 */
#define XFS_INODE_CHUNK         16      /* 每次分配 16 个 inode 为一组 */
#define XFS_ROOT_INO            128     /* 根目录 inode 号（与 mkfs.xfs 兼容）*/

/* 文件类型 */
#define XFS_DIR_FMT_SF          1       /* shortform 目录 */
#define XFS_DIR_FMT_BLOCK       2       /* 块目录 */

/*
 * ============================================================
 * XFS 超级块（512字节对齐，位于 AG0 block 0）
 *
 * 参考：fs/xfs/libxfs/xfs_format.h struct xfs_dsb
 * ============================================================
 */
struct xfs_dsb {
    u32  sb_magicnum;       /* XFS_SB_MAGIC（大端序）*/
    u32  sb_blocksize;      /* 块大小（4096，大端序）*/
    u64  sb_dblocks;        /* 数据块总数（大端序）*/
    u64  sb_logstart;       /* 日志起始块号（大端序）*/
    u64  sb_rootino;        /* 根目录 inode 号（大端序）*/
    u32  sb_agblocks;       /* 每个 AG 块数（大端序）*/
    u32  sb_agcount;        /* AG 数量（大端序）*/
    u32  sb_logblocks;      /* 日志块数（大端序）*/
    u16  sb_sectsize;       /* 扇区大小（大端序）*/
    u16  sb_inodesize;      /* inode 大小（大端序）*/
    u16  sb_inopblock;      /* 每块 inode 数（大端序）*/
    u8   sb_blocklog;       /* log2(blocksize) */
    u8   sb_inodelog;       /* log2(inodesize) */
    u32  sb_icount;         /* 已分配 inode 数（大端序）*/
    u32  sb_ifree;          /* 空闲 inode 数（大端序）*/
    u32  sb_fdblocks;       /* 空闲数据块数（大端序）*/
    u8   sb_pad[512 - 62];  /* 填充到 512 字节 */
};

/*
 * ============================================================
 * AG Free Space 头（AGF）
 *
 * 位于每个 AG 的 block 1。管理空闲空间 B+树。
 *
 * 参考：fs/xfs/libxfs/xfs_format.h struct xfs_agf
 * ============================================================
 */
struct xfs_agf {
    u32  agf_magicnum;      /* XFS_AGF_MAGIC（大端序）*/
    u32  agf_seqno;         /* AG 序号（大端序）*/
    u32  agf_length;        /* AG 块数（大端序）*/
    u32  agf_bno_root;      /* bnobt 根块号（大端序）*/
    u32  agf_cnt_root;      /* cntbt 根块号（大端序）*/
    u32  agf_bno_level;     /* bnobt 层数（大端序）*/
    u32  agf_cnt_level;     /* cntbt 层数（大端序）*/
    u32  agf_freeblks;      /* 空闲块数（大端序）*/
    u8   agf_pad[512 - 32]; /* 填充到 512 字节 */
};

/*
 * ============================================================
 * AG Inode 管理头（AGI）
 *
 * 位于每个 AG 的 block 2（偏移 1024 字节）。
 *
 * 参考：fs/xfs/libxfs/xfs_format.h struct xfs_agi
 * ============================================================
 */
struct xfs_agi {
    u32  agi_magicnum;      /* XFS_AGI_MAGIC（大端序）*/
    u32  agi_seqno;         /* AG 序号（大端序）*/
    u32  agi_length;        /* AG 块数（大端序）*/
    u32  agi_count;         /* AG 内已分配 inode 数（大端序）*/
    u32  agi_root;          /* inobt 根块号（大端序）*/
    u32  agi_level;         /* inobt 层数（大端序）*/
    u32  agi_freecount;     /* AG 内空闲 inode 数（大端序）*/
    u32  agi_newino;        /* 最近分配的 inode 块号（大端序）*/
    u8   agi_pad[512 - 32]; /* 填充到 512 字节 */
};

/*
 * ============================================================
 * 空闲空间 B+树记录
 *
 * bnobt：按起始块号排序
 * cntbt：按块数排序
 *
 * 参考：fs/xfs/libxfs/xfs_alloc_btree.h
 * ============================================================
 */
struct xfs_alloc_rec {
    u32  ar_startblock;     /* AG 内起始块号（大端序）*/
    u32  ar_blockcount;     /* 连续空闲块数（大端序）*/
};

/*
 * ============================================================
 * B+树块头（简化：仅叶节点）
 *
 * 参考：fs/xfs/libxfs/xfs_btree.h struct xfs_btree_block
 * ============================================================
 */
struct xfs_btree_block {
    u32  bb_magic;          /* 魔数（大端序）*/
    u16  bb_level;          /* 层级（0=叶节点，大端序）*/
    u16  bb_numrecs;        /* 记录数（大端序）*/
    /* 记录紧随头部之后 */
};

/*
 * ============================================================
 * XFS inode 磁盘格式（256字节）
 *
 * 参考：fs/xfs/libxfs/xfs_format.h struct xfs_dinode
 * ============================================================
 */

/* inode 核心（dinode core） */
#define XFS_DINODE_MAGIC    0x494e      /* "IN"（大端序）*/

/* 文件类型（与 POSIX 一致） */
#define XFS_DINODE_FMT_REG      S_IFREG
#define XFS_DINODE_FMT_DIR      S_IFDIR

/* 数据 fork 格式 */
#define XFS_DINODE_FMT_LOCAL    1       /* 数据内嵌在 inode 中 */
#define XFS_DINODE_FMT_EXTENTS  2       /* 数据在 extent 列表中 */

struct xfs_dinode {
    u16  di_magic;          /* XFS_DINODE_MAGIC（大端序）*/
    u16  di_mode;           /* 文件类型+权限（大端序）*/
    u32  di_uid;            /* 用户 ID（大端序）*/
    u32  di_gid;            /* 组 ID（大端序）*/
    u32  di_nlink;          /* 硬链接数（大端序）*/
    u64  di_size;           /* 文件大小（大端序）*/
    u64  di_nblocks;        /* 数据块数（大端序）*/
    u8   di_format;         /* 数据 fork 格式 */
    u8   di_pad1;
    u16  di_pad2;
    u32  di_gen;            /* 版本号（大端序）*/
    /* 以下为数据 fork 区域（176 字节可用）*/
    u8   di_datafork[176];  /* shortform dir 或 extent list */
    u8   di_pad3[256 - 48 - 176]; /* 填充到 256 字节 */
};

/*
 * ============================================================
 * Extent 记录（用于文件数据块定位）
 * ============================================================
 */
struct xfs_bmbt_rec {
    u64  br_startoff;       /* 文件逻辑块偏移（大端序）*/
    u64  br_startblock;     /* 磁盘物理块号（大端序）*/
    u32  br_blockcount;     /* 连续块数（大端序）*/
    u32  br_pad;
};

/*
 * ============================================================
 * shortform 目录（小目录，数据内嵌在 inode 中）
 *
 * 参考：fs/xfs/libxfs/xfs_dir2_sf.h
 * ============================================================
 */
struct xfs_dir2_sf_hdr {
    u32  count;             /* 条目数（大端序）*/
    u64  parent;            /* 父目录 inode 号（大端序）*/
};

struct xfs_dir2_sf_entry {
    u64  inumber;           /* inode 号（大端序）*/
    u8   namelen;           /* 文件名长度 */
    u8   ftype;             /* 文件类型 */
    /* char name[]; — 紧随其后 */
};

/*
 * ============================================================
 * WAL 日志记录
 *
 * 参考：fs/xfs/xfs_log_format.h struct xlog_rec_header
 * ============================================================
 */
struct xfs_log_record {
    u32  h_magicno;         /* XFS_LOG_MAGIC（大端序）*/
    u32  h_len;             /* 记录总长度（大端序）*/
    u64  h_lsn;             /* Log Sequence Number（大端序）*/
    u32  h_type;            /* 日志项类型（大端序）*/
    u32  h_num_logops;      /* 操作数（大端序）*/
};

/* 日志项类型 */
#define XFS_LOG_INODE_CREATE    1
#define XFS_LOG_INODE_UPDATE    2
#define XFS_LOG_DIR_ADD         3
#define XFS_LOG_ALLOC           4
#define XFS_LOG_COMMIT          5

/*
 * ============================================================
 * 内存中的 XFS 挂载信息
 * ============================================================
 */
struct xfs_mount {
    u32  m_agcount;         /* AG 数量 */
    u32  m_agblocks;        /* 每 AG 块数 */
    u32  m_blocksize;       /* 块大小 */
    u64  m_dblocks;         /* 总块数 */
    u64  m_logstart;        /* 日志起始块 */
    u32  m_logblocks;       /* 日志块数 */
    u64  m_rootino;         /* 根 inode 号 */
    u32  m_icount;          /* 已分配 inode 数 */
    u32  m_ifree;           /* 空闲 inode 数 */
    u32  m_fdblocks;        /* 空闲块数 */
    u64  m_log_lsn;         /* 当前日志 LSN */
    u32  m_log_head;        /* 日志写入头（块偏移）*/
    struct super_block *m_sb; /* VFS 超级块 */
};

/* XFS inode 内存中私有数据 */
struct xfs_inode_info {
    u64  ino;               /* inode 号 */
    u8   di_format;         /* 数据 fork 格式 */
    u64  data_startblock;   /* 数据起始块号（extent 模式）*/
    u32  data_blockcount;   /* 数据块数 */
    /* shortform dir 缓存 */
    u32  dir_count;         /* 目录条目数 */
    u8   dir_data[176];     /* shortform 目录数据副本 */
};

/*
 * ============================================================
 * 函数声明
 * ============================================================
 */

/* xfs_super.c */
void xfs_init(void);
void xfs_mkfs(void);

/* xfs_log.c */
void xfs_log_init(struct xfs_mount *mp);
int  xfs_log_write_record(struct xfs_mount *mp, u32 type,
                           const void *data, u32 data_len);
int  xfs_log_commit(struct xfs_mount *mp);
int  xfs_log_recover(struct xfs_mount *mp);

/* xfs_inode.c */
struct inode *xfs_iget(struct super_block *sb, u64 ino);
int  xfs_inode_write(struct xfs_mount *mp, u64 ino, struct xfs_dinode *dip);
int  xfs_inode_read(struct xfs_mount *mp, u64 ino, struct xfs_dinode *dip);

/* xfs_alloc.c */
int  xfs_alloc_block(struct xfs_mount *mp, u32 agno, u32 *bno_out);
void xfs_free_block(struct xfs_mount *mp, u32 agno, u32 bno);
void xfs_alloc_init_ag(struct xfs_mount *mp, u32 agno,
                        u32 first_free, u32 ag_length);

/* xfs_dir2.c */
struct dentry *xfs_dir_lookup(struct inode *dir, struct dentry *dentry,
                               unsigned int flags);
int  xfs_dir_create(struct inode *dir, struct dentry *dentry,
                     unsigned int mode, bool excl);
int  xfs_dir_add_entry(struct xfs_mount *mp, u64 dir_ino,
                        u64 child_ino, const char *name, u8 namelen,
                        u8 ftype);

#endif /* __LINUX_XFS_FORMAT_H */
