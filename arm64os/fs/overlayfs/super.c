/* SPDX-License-Identifier: GPL-2.0 */
/*
 * arm64os/fs/overlayfs/super.c
 *
 * overlayfs 超级块与挂载
 *
 * 参考：fs/overlayfs/super.c
 *
 * Phase 9 实现：
 *   - ovl_mount()：解析 lowerdir/upperdir/workdir 挂载选项
 *   - ovl_fill_super()：查找各层挂载点，构建 overlay 超级块
 *   - ovl_init()：注册 overlay 文件系统类型
 *
 * 简化说明：
 *   - 仅支持单个 lower 层（不支持多层 lower）
 *   - lower、upper、work 必须是已挂载的路径或根文件系统子目录
 *   - 选项字符串格式："lowerdir=/sq,upperdir=/upper,workdir=/work"
 */

#include <linux/types.h>
#include <linux/list.h>
#include <linux/fs.h>

/* 外部函数 */
void boot_printk(const char *s);
void boot_printk_hex(unsigned long val);

/* VFS 接口 */
struct super_block *alloc_super(struct file_system_type *type);
struct inode *new_inode(struct super_block *sb);
struct dentry *d_alloc_root(struct super_block *sb);
int register_filesystem(struct file_system_type *fs);
struct dentry *path_lookup(const char *pathname);

/* overlayfs inode 操作（由 inode.c 提供） */
extern const struct inode_operations ovl_dir_inode_ops;
extern const struct file_operations  ovl_dir_fops;

/*
 * ============================================================
 * overlayfs 内部数据结构
 * ============================================================
 */

/* overlayfs 魔数 */
#define OVL_SUPER_MAGIC     0x794c7630UL    /* "ovl0" */

/*
 * ovl_fs - overlayfs 超级块私有数据
 *
 * 保存三层目录的 root dentry 引用。
 *
 * 参考：fs/overlayfs/ovl_entry.h struct ovl_fs
 */
struct ovl_fs {
    struct dentry  *lower_root;     /* lower 层根 dentry（squashfs） */
    struct dentry  *upper_root;     /* upper 层根 dentry（ramfs 子目录） */
    struct dentry  *work_root;      /* work 目录根 dentry */
};

/* ovl_fs 静态池 */
static struct ovl_fs ovl_fs_pool[2];
static int ovl_fs_idx = 0;

/*
 * ============================================================
 * 内部辅助函数：选项字符串解析
 * ============================================================
 */

/*
 * ovl_str_ncmp - 比较前 n 个字符
 */
static int ovl_str_ncmp(const char *a, const char *b, int n)
{
    int i;
    for (i = 0; i < n; i++) {
        if (a[i] != b[i])
            return a[i] - b[i];
        if (a[i] == '\0')
            return 0;
    }
    return 0;
}

/*
 * ovl_str_copy - 复制字符串
 */
static void ovl_str_copy(char *dst, const char *src, int max)
{
    int i;
    for (i = 0; i < max - 1 && src[i]; i++)
        dst[i] = src[i];
    dst[i] = '\0';
}

/*
 * ovl_parse_option - 解析单个选项 "key=value"
 *
 * 在 opts 字符串中查找 key=，提取 value 到 buf。
 * value 以 ',' 或 '\0' 结尾。
 *
 * @opts: 选项字符串（如 "lowerdir=/sq,upperdir=/upper,workdir=/work"）
 * @key:  选项名（如 "lowerdir="）
 * @klen: 选项名长度（含 '='）
 * @buf:  输出缓冲区
 * @blen: 缓冲区大小
 *
 * 返回 1 找到，0 未找到。
 */
static int ovl_parse_option(const char *opts, const char *key, int klen,
                             char *buf, int blen)
{
    const char *p = opts;
    int i;

    if (!opts)
        return 0;

    while (*p) {
        /* 检查是否匹配 key */
        if (ovl_str_ncmp(p, key, klen) == 0) {
            /* 提取 value */
            p += klen;
            for (i = 0; i < blen - 1 && p[i] && p[i] != ','; i++)
                buf[i] = p[i];
            buf[i] = '\0';
            return 1;
        }

        /* 跳到下一个选项 */
        while (*p && *p != ',')
            p++;
        if (*p == ',')
            p++;
    }

    return 0;
}

/*
 * ============================================================
 * ovl_fill_super - 填充 overlayfs 超级块
 *
 * 解析挂载选项，查找各层路径对应的 dentry，
 * 创建 overlay 根目录 inode。
 *
 * @sb:   已分配的 VFS 超级块
 * @data: 挂载选项字符串
 *
 * 返回 0 成功，负数失败。
 *
 * 参考：fs/overlayfs/super.c ovl_fill_super()
 * ============================================================
 */
static int ovl_fill_super(struct super_block *sb, void *data)
{
    struct ovl_fs *ofs;
    struct inode *root_inode;
    struct dentry *root_dentry;
    char lower_path[32];
    char upper_path[32];
    char work_path[32];

    /* 分配 ovl_fs */
    if (ovl_fs_idx >= 2) {
        boot_printk("[overlayfs] ERROR: ovl_fs pool exhausted\n");
        return -1;
    }
    ofs = &ovl_fs_pool[ovl_fs_idx++];

    /* 解析挂载选项 */
    if (!ovl_parse_option((const char *)data, "lowerdir=", 9,
                           lower_path, 32)) {
        boot_printk("[overlayfs] ERROR: missing lowerdir option\n");
        return -1;
    }
    if (!ovl_parse_option((const char *)data, "upperdir=", 9,
                           upper_path, 32)) {
        boot_printk("[overlayfs] ERROR: missing upperdir option\n");
        return -1;
    }
    if (!ovl_parse_option((const char *)data, "workdir=", 8,
                           work_path, 32)) {
        boot_printk("[overlayfs] ERROR: missing workdir option\n");
        return -1;
    }

    boot_printk("[overlayfs] lower=");
    boot_printk(lower_path);
    boot_printk(" upper=");
    boot_printk(upper_path);
    boot_printk(" work=");
    boot_printk(work_path);
    boot_printk("\n");

    /* 查找各层根 dentry */
    ofs->lower_root = path_lookup(lower_path);
    if (!ofs->lower_root || !ofs->lower_root->d_inode) {
        boot_printk("[overlayfs] ERROR: lower path not found: ");
        boot_printk(lower_path);
        boot_printk("\n");
        return -1;
    }

    ofs->upper_root = path_lookup(upper_path);
    if (!ofs->upper_root || !ofs->upper_root->d_inode) {
        boot_printk("[overlayfs] ERROR: upper path not found: ");
        boot_printk(upper_path);
        boot_printk("\n");
        return -1;
    }

    ofs->work_root = path_lookup(work_path);
    if (!ofs->work_root || !ofs->work_root->d_inode) {
        boot_printk("[overlayfs] ERROR: work path not found: ");
        boot_printk(work_path);
        boot_printk("\n");
        return -1;
    }

    /* 填充 VFS 超级块 */
    sb->s_magic = OVL_SUPER_MAGIC;
    sb->s_blocksize = 4096;
    sb->s_fs_info = ofs;

    /* 创建 overlay 根目录 inode */
    root_inode = new_inode(sb);
    if (!root_inode) {
        boot_printk("[overlayfs] ERROR: cannot create root inode\n");
        return -1;
    }

    root_inode->i_mode = S_IFDIR | 0755;
    root_inode->i_op = &ovl_dir_inode_ops;
    root_inode->i_fop = &ovl_dir_fops;

    /*
     * 根 inode 的 i_private 指向 ovl_fs，
     * 以便 lookup 时能找到 lower/upper 层。
     * 非根 inode 使用 ovl_inode_info（由 inode.c 分配）。
     */
    root_inode->i_private = ofs;

    /* 创建根 dentry */
    root_dentry = d_alloc_root(sb);
    if (!root_dentry) {
        boot_printk("[overlayfs] ERROR: cannot create root dentry\n");
        return -1;
    }
    root_dentry->d_inode = root_inode;
    sb->s_root = root_dentry;

    boot_printk("[overlayfs] Superblock filled, root ready\n");

    return 0;
}

/*
 * ============================================================
 * ovl_mount - overlayfs mount 回调
 *
 * 参考：fs/overlayfs/super.c ovl_mount()
 * ============================================================
 */
static struct dentry *ovl_mount(struct file_system_type *fs_type,
                                 int flags, const char *dev_name, void *data)
{
    struct super_block *sb;
    int ret;

    boot_printk("[overlayfs] Mounting overlayfs...\n");

    sb = alloc_super(fs_type);
    if (!sb)
        return NULL;

    ret = ovl_fill_super(sb, data);
    if (ret != 0)
        return NULL;

    boot_printk("[overlayfs] overlayfs mounted successfully\n");

    return sb->s_root;
}

/*
 * ============================================================
 * ovl_kill_sb - 卸载回调
 * ============================================================
 */
static void ovl_kill_sb(struct super_block *sb)
{
    boot_printk("[overlayfs] Unmounting overlayfs\n");
    (void)sb;
}

/*
 * ============================================================
 * 文件系统类型注册
 * ============================================================
 */
static struct file_system_type ovl_fs_type = {
    .name    = "overlay",
    .mount   = ovl_mount,
    .kill_sb = ovl_kill_sb,
    .next    = NULL,
};

/*
 * ============================================================
 * ovl_init - 注册 overlay 文件系统
 *
 * 在 VFS 初始化后调用。
 *
 * 参考：fs/overlayfs/super.c init_overlayfs()
 * ============================================================
 */
void ovl_init(void)
{
    register_filesystem(&ovl_fs_type);
}
