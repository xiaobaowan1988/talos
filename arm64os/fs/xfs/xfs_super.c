/* SPDX-License-Identifier: GPL-2.0 */
/*
 * arm64os/fs/xfs/xfs_super.c
 *
 * XFS 超级块、挂载、mkfs
 *
 * 参考：fs/xfs/xfs_super.c
 *       fs/xfs/xfs_mount.c
 *
 * Phase 8 教学简化版：
 *   - 2 个 AG（Allocation Group）
 *   - 每个 AG 固定块数
 *   - AG 0 包含 WAL 日志
 *   - mkfs 格式化磁盘分区
 */

#include <linux/types.h>
#include <linux/fs.h>
#include <linux/xfs_format.h>

/* 外部函数 */
void boot_printk(const char *s);
void boot_printk_hex(unsigned long val);
int virtio_blk_read(u64 sector, void *buf, u32 len);
int virtio_blk_write(u64 sector, const void *buf, u32 len);
struct super_block *alloc_super(struct file_system_type *type);
struct inode *xfs_iget(struct super_block *sb, u64 ino);
struct dentry *d_alloc_root(struct super_block *sb);
int register_filesystem(struct file_system_type *fs);
void xfs_log_init(struct xfs_mount *mp);
int xfs_log_recover(struct xfs_mount *mp);
void xfs_alloc_init_ag(struct xfs_mount *mp, u32 agno,
                        u32 first_free, u32 ag_length);

/*
 * ============================================================
 * 内部辅助
 * ============================================================
 */
static void xfs_super_mem_zero(void *dst, u32 len)
{
    u8 *p = (u8 *)dst;
    u32 i;
    for (i = 0; i < len; i++)
        p[i] = 0;
}

static void xfs_super_mem_copy(void *dst, const void *src, u32 len)
{
    u8 *d = (u8 *)dst;
    const u8 *s = (const u8 *)src;
    u32 i;
    for (i = 0; i < len; i++)
        d[i] = s[i];
}

/* xfs_mount 静态池 */
static struct xfs_mount xfs_mp_pool[2];
static int xfs_mp_idx = 0;

/*
 * ============================================================
 * xfs_fill_super - 从磁盘读取超级块并填充 VFS super_block
 *
 * @sb: 已分配的 VFS 超级块
 *
 * 返回 0 成功，负数失败。
 *
 * 参考：fs/xfs/xfs_super.c xfs_fs_fill_super()
 * ============================================================
 */
static int xfs_fill_super(struct super_block *sb)
{
    struct xfs_dsb dsb;
    struct xfs_mount *mp;
    struct inode *root_inode;
    struct dentry *root_dentry;
    u32 ag_header_blocks;
    static u8 __attribute__((aligned(4096))) io_buf[512];

    /* 从分区起始读取超级块 */
    if (virtio_blk_read(XFS_PART_START, io_buf, 512) != 0) {
        boot_printk("[xfs] ERROR: read superblock failed\n");
        return -1;
    }

    xfs_super_mem_copy(&dsb, io_buf, sizeof(dsb));

    /* 验证魔数（大端序） */
    if (be32_to_cpu(dsb.sb_magicnum) != XFS_SB_MAGIC) {
        boot_printk("[xfs] ERROR: bad magic ");
        boot_printk_hex(be32_to_cpu(dsb.sb_magicnum));
        boot_printk("\n");
        return -1;
    }

    boot_printk("[xfs] superblock: magic=XFSB");
    boot_printk(" agcount=");
    boot_printk_hex(be32_to_cpu(dsb.sb_agcount));
    boot_printk(" agblocks=");
    boot_printk_hex(be32_to_cpu(dsb.sb_agblocks));
    boot_printk("\n");

    /* 分配 xfs_mount */
    if (xfs_mp_idx >= 2) {
        boot_printk("[xfs] ERROR: mount pool exhausted\n");
        return -1;
    }
    mp = &xfs_mp_pool[xfs_mp_idx++];

    /* 解析超级块字段（大端序转换） */
    mp->m_agcount = be32_to_cpu(dsb.sb_agcount);
    mp->m_agblocks = be32_to_cpu(dsb.sb_agblocks);
    mp->m_blocksize = be32_to_cpu(dsb.sb_blocksize);
    mp->m_dblocks = be64_to_cpu(dsb.sb_dblocks);
    mp->m_logstart = be64_to_cpu(dsb.sb_logstart);
    mp->m_logblocks = be32_to_cpu(dsb.sb_logblocks);
    mp->m_rootino = be64_to_cpu(dsb.sb_rootino);
    mp->m_icount = be32_to_cpu(dsb.sb_icount);
    mp->m_ifree = be32_to_cpu(dsb.sb_ifree);
    mp->m_fdblocks = be32_to_cpu(dsb.sb_fdblocks);
    mp->m_sb = sb;

    /* 填充 VFS super_block */
    sb->s_magic = XFS_SB_MAGIC;
    sb->s_blocksize = mp->m_blocksize;
    sb->s_fs_info = mp;

    /* 初始化日志 */
    xfs_log_init(mp);

    /* 日志恢复 */
    xfs_log_recover(mp);

    /* 初始化 AG 空闲空间管理 */
    ag_header_blocks = 5 + mp->m_logblocks;  /* AG header + log */
    {
        u32 i;
        for (i = 0; i < mp->m_agcount; i++) {
            u32 first_free;
            if (i == 0) {
                /* AG 0: header blocks + log + 1 inode block (for root) */
                first_free = ag_header_blocks + 1;
            } else {
                first_free = 5;  /* AG 1+: just AG header blocks (no log) */
            }
            xfs_alloc_init_ag(mp, i, first_free, mp->m_agblocks);
        }
    }

    /* 读取根 inode */
    root_inode = xfs_iget(sb, mp->m_rootino);
    if (!root_inode) {
        boot_printk("[xfs] ERROR: read root inode failed\n");
        return -1;
    }

    /* 创建根 dentry */
    root_dentry = d_alloc_root(sb);
    if (!root_dentry)
        return -1;
    root_dentry->d_inode = root_inode;
    sb->s_root = root_dentry;

    boot_printk("[xfs] mounted: ");
    boot_printk_hex((unsigned long)mp->m_dblocks);
    boot_printk(" blocks, root_ino=");
    boot_printk_hex((unsigned long)mp->m_rootino);
    boot_printk("\n");

    return 0;
}

/*
 * ============================================================
 * xfs_mount_cb - 文件系统 mount 回调
 * ============================================================
 */
static struct dentry *xfs_mount_cb(struct file_system_type *fs_type,
                                     int flags, const char *dev_name,
                                     void *data)
{
    struct super_block *sb;
    int ret;

    boot_printk("[xfs] Mounting XFS...\n");

    sb = alloc_super(fs_type);
    if (!sb)
        return NULL;

    ret = xfs_fill_super(sb);
    if (ret != 0)
        return NULL;

    return sb->s_root;
}

/*
 * ============================================================
 * xfs_kill_sb - 卸载回调
 * ============================================================
 */
static void xfs_kill_sb(struct super_block *sb)
{
    boot_printk("[xfs] Unmounting XFS\n");
    (void)sb;
}

/*
 * ============================================================
 * 文件系统类型注册
 * ============================================================
 */
static struct file_system_type xfs_fs_type = {
    .name    = "xfs",
    .mount   = xfs_mount_cb,
    .kill_sb = xfs_kill_sb,
    .next    = NULL,
};

/*
 * ============================================================
 * xfs_init - 注册 XFS 文件系统
 * ============================================================
 */
void xfs_init(void)
{
    register_filesystem(&xfs_fs_type);
}

/*
 * ============================================================
 * xfs_mkfs - 格式化 XFS 文件系统
 *
 * 在磁盘 XFS 分区上构建文件系统结构：
 *   1. AG0 超级块
 *   2. AG0/AG1 的 AGF、AGI 头
 *   3. 初始化空闲空间 B+树
 *   4. 创建根目录 inode
 *   5. 初始化空的 WAL 日志区域
 *
 * 参考：Phase 8 设计文档 §8.8
 * ============================================================
 */
void xfs_mkfs(void)
{
    static u8 __attribute__((aligned(4096))) buf[4096];
    struct xfs_dsb *dsb;
    struct xfs_agf *agf;
    struct xfs_agi *agi;
    struct xfs_btree_block *bnobt;
    struct xfs_alloc_rec *frec;
    struct xfs_dinode *dip;
    struct xfs_dir2_sf_hdr *sfh;
    u32 total_blocks;
    u32 ag_blocks;
    u32 log_start;
    u32 inode_start;
    u32 ag1_start;
    u32 i;

    boot_printk("[xfs] Formatting XFS...\n");

    /*
     * 分区参数：
     *   分区大小 = XFS_PART_SECTORS * 512 = 5MB
     *   块大小 = 4096
     *   总块数 = 5MB / 4KB = 1280
     *   AG 数量 = 2
     *   每 AG 块数 = 640
     *
     * AG 0 布局（块号）：
     *   block 0: superblock
     *   block 1: AGF
     *   block 2: AGI
     *   block 3: bnobt root (free space B+tree)
     *   block 4: inobt root (inode B+tree)
     *   block 5-36: WAL log (32 blocks)
     *   block 37: root inode block (inode 128)
     *   block 38+: free data blocks
     */
    total_blocks = XFS_PART_SECTORS / (XFS_BLOCK_SIZE / 512);  /* 1280 */
    ag_blocks = total_blocks / XFS_AG_COUNT;                    /* 640 */
    log_start = 5;                                               /* block 5 */
    inode_start = 5 + XFS_LOG_BLOCKS;                           /* block 37 */
    ag1_start = ag_blocks;                                       /* block 640 */

    /* ---- Step 1: Write superblock (AG0 block 0) ---- */
    xfs_super_mem_zero(buf, XFS_BLOCK_SIZE);
    dsb = (struct xfs_dsb *)buf;
    dsb->sb_magicnum = cpu_to_be32(XFS_SB_MAGIC);
    dsb->sb_blocksize = cpu_to_be32(XFS_BLOCK_SIZE);
    dsb->sb_dblocks = cpu_to_be64((u64)total_blocks);
    dsb->sb_logstart = cpu_to_be64((u64)log_start);
    dsb->sb_rootino = cpu_to_be64((u64)XFS_ROOT_INO);
    dsb->sb_agblocks = cpu_to_be32(ag_blocks);
    dsb->sb_agcount = cpu_to_be32(XFS_AG_COUNT);
    dsb->sb_logblocks = cpu_to_be32(XFS_LOG_BLOCKS);
    dsb->sb_sectsize = cpu_to_be16(XFS_SECTOR_SIZE);
    dsb->sb_inodesize = cpu_to_be16(XFS_INODE_SIZE);
    dsb->sb_inopblock = cpu_to_be16(XFS_INODES_PER_BLOCK);
    dsb->sb_blocklog = XFS_BLOCK_LOG;
    dsb->sb_inodelog = XFS_INODE_LOG;
    dsb->sb_icount = cpu_to_be32(1);       /* root inode */
    dsb->sb_ifree = cpu_to_be32(XFS_INODES_PER_BLOCK - 1);
    dsb->sb_fdblocks = cpu_to_be32(total_blocks - inode_start - 1 - 5);

    virtio_blk_write(XFS_PART_START, buf, 512);

    /* ---- Step 2: Write AG0 AGF (block 1) ---- */
    xfs_super_mem_zero(buf, 512);
    agf = (struct xfs_agf *)buf;
    agf->agf_magicnum = cpu_to_be32(XFS_AGF_MAGIC);
    agf->agf_seqno = cpu_to_be32(0);
    agf->agf_length = cpu_to_be32(ag_blocks);
    agf->agf_bno_root = cpu_to_be32(3);    /* block 3 = bnobt root */
    agf->agf_cnt_root = cpu_to_be32(3);    /* same block (simplified) */
    agf->agf_bno_level = cpu_to_be32(1);
    agf->agf_cnt_level = cpu_to_be32(1);
    agf->agf_freeblks = cpu_to_be32(ag_blocks - inode_start - 1);

    virtio_blk_write(XFS_PART_START + 1 * (XFS_BLOCK_SIZE / 512), buf, 512);

    /* ---- Step 3: Write AG0 AGI (block 2) ---- */
    xfs_super_mem_zero(buf, 512);
    agi = (struct xfs_agi *)buf;
    agi->agi_magicnum = cpu_to_be32(XFS_AGI_MAGIC);
    agi->agi_seqno = cpu_to_be32(0);
    agi->agi_length = cpu_to_be32(ag_blocks);
    agi->agi_count = cpu_to_be32(XFS_INODES_PER_BLOCK);
    agi->agi_root = cpu_to_be32(4);         /* block 4 = inobt root */
    agi->agi_level = cpu_to_be32(1);
    agi->agi_freecount = cpu_to_be32(XFS_INODES_PER_BLOCK - 1);
    agi->agi_newino = cpu_to_be32(inode_start);

    virtio_blk_write(XFS_PART_START + 2 * (XFS_BLOCK_SIZE / 512), buf, 512);

    /* ---- Step 4: Write AG0 bnobt (block 3) ---- */
    xfs_super_mem_zero(buf, XFS_BLOCK_SIZE);
    bnobt = (struct xfs_btree_block *)buf;
    bnobt->bb_magic = cpu_to_be32(XFS_ABTB_MAGIC);
    bnobt->bb_level = cpu_to_be16(0);      /* leaf */
    bnobt->bb_numrecs = cpu_to_be16(1);

    /* 一个大空闲区：从 inode_start+1 到 AG 末尾 */
    frec = (struct xfs_alloc_rec *)(buf + sizeof(struct xfs_btree_block));
    frec->ar_startblock = cpu_to_be32(inode_start + 1);
    frec->ar_blockcount = cpu_to_be32(ag_blocks - inode_start - 1);

    virtio_blk_write(XFS_PART_START + 3 * (XFS_BLOCK_SIZE / 512),
                      buf, XFS_BLOCK_SIZE);

    /* ---- Step 5: Zero out log area (blocks 5-36) ---- */
    xfs_super_mem_zero(buf, XFS_BLOCK_SIZE);
    for (i = 0; i < XFS_LOG_BLOCKS; i++) {
        virtio_blk_write(XFS_PART_START +
                          (u64)(log_start + i) * (XFS_BLOCK_SIZE / 512),
                          buf, XFS_BLOCK_SIZE);
    }

    /* ---- Step 6: Write root inode (block inode_start) ---- */
    xfs_super_mem_zero(buf, XFS_BLOCK_SIZE);

    /* 根目录 inode (slot 0 = inode 128) */
    dip = (struct xfs_dinode *)buf;
    dip->di_magic = cpu_to_be16(XFS_DINODE_MAGIC);
    dip->di_mode = cpu_to_be16(S_IFDIR | 0755);
    dip->di_uid = cpu_to_be32(0);
    dip->di_gid = cpu_to_be32(0);
    dip->di_nlink = cpu_to_be32(2);     /* . and .. */
    dip->di_size = cpu_to_be64(sizeof(struct xfs_dir2_sf_hdr));
    dip->di_nblocks = cpu_to_be64(0);
    dip->di_format = XFS_DINODE_FMT_LOCAL;
    dip->di_gen = cpu_to_be32(1);

    /* shortform 目录：空（仅有 header） */
    sfh = (struct xfs_dir2_sf_hdr *)dip->di_datafork;
    sfh->count = cpu_to_be32(0);
    sfh->parent = cpu_to_be64(XFS_ROOT_INO);   /* parent = self */

    virtio_blk_write(XFS_PART_START +
                      (u64)inode_start * (XFS_BLOCK_SIZE / 512),
                      buf, XFS_BLOCK_SIZE);

    /* ---- Step 7: Write AG1 headers ---- */
    /* AG1 AGF */
    xfs_super_mem_zero(buf, 512);
    agf = (struct xfs_agf *)buf;
    agf->agf_magicnum = cpu_to_be32(XFS_AGF_MAGIC);
    agf->agf_seqno = cpu_to_be32(1);
    agf->agf_length = cpu_to_be32(ag_blocks);
    agf->agf_bno_root = cpu_to_be32(3);
    agf->agf_cnt_root = cpu_to_be32(3);
    agf->agf_bno_level = cpu_to_be32(1);
    agf->agf_cnt_level = cpu_to_be32(1);
    agf->agf_freeblks = cpu_to_be32(ag_blocks - 5);

    virtio_blk_write(XFS_PART_START +
                      (u64)(ag1_start + 1) * (XFS_BLOCK_SIZE / 512),
                      buf, 512);

    /* AG1 AGI */
    xfs_super_mem_zero(buf, 512);
    agi = (struct xfs_agi *)buf;
    agi->agi_magicnum = cpu_to_be32(XFS_AGI_MAGIC);
    agi->agi_seqno = cpu_to_be32(1);
    agi->agi_length = cpu_to_be32(ag_blocks);
    agi->agi_count = cpu_to_be32(0);
    agi->agi_root = cpu_to_be32(4);
    agi->agi_level = cpu_to_be32(1);
    agi->agi_freecount = cpu_to_be32(0);

    virtio_blk_write(XFS_PART_START +
                      (u64)(ag1_start + 2) * (XFS_BLOCK_SIZE / 512),
                      buf, 512);

    boot_printk("[xfs] Formatted: ");
    boot_printk_hex(total_blocks);
    boot_printk(" blocks, ");
    boot_printk_hex(XFS_AG_COUNT);
    boot_printk(" AGs, log=");
    boot_printk_hex(XFS_LOG_BLOCKS);
    boot_printk(" blocks\n");
}
