/* SPDX-License-Identifier: GPL-2.0 */
/*
 * arm64os/fs/vfs/super.c
 *
 * 超级块管理、文件系统注册与挂载
 *
 * 参考：fs/super.c
 *       fs/filesystems.c
 *       fs/namespace.c
 *
 * Phase 7 实现：
 *   - register_filesystem()：注册文件系统类型（如 "ramfs"）
 *   - get_fs_type()：按名称查找已注册的文件系统类型
 *   - alloc_super()：分配并初始化超级块
 *   - do_mount()：挂载文件系统到指定路径
 *   - vfs_init()：VFS 子系统初始化入口
 *
 * 简化说明：
 *   - 超级块使用静态池（MAX_SUPERBLOCKS = 4）
 *   - 文件系统类型以单链表管理
 *   - 仅支持根挂载（"/"），不支持 bind mount / overlay 等
 */

#include <linux/types.h>
#include <linux/list.h>
#include <linux/fs.h>

/* 外部函数 */
void boot_printk(const char *s);
void boot_printk_hex(unsigned long val);

/* 其他 VFS 子系统初始化 */
void dcache_init(void);
void inode_init(void);
void files_init(void);

/*
 * ============================================================
 * 全局状态
 * ============================================================
 */

/* 超级块静态池 */
static struct super_block sb_pool[MAX_SUPERBLOCKS];
static int sb_pool_idx = 0;

/* 已注册文件系统类型链表 */
static struct file_system_type *file_systems = NULL;

/* 全局根挂载点 */
struct vfsmount root_mnt_storage;
struct vfsmount *root_mnt = NULL;

/* 全局 init 进程文件描述符表 */
struct files_struct init_files;

/*
 * ============================================================
 * vfs_init - VFS 子系统初始化
 *
 * 在 start_kernel() 中调用，初始化所有 VFS 子系统：
 *   1. dcache（dentry hash 表）
 *   2. inode 池
 *   3. file 池
 *   4. init_files（内核初始文件描述符表）
 *
 * 参考：fs/dcache.c vfs_caches_init()
 * ============================================================
 */
void vfs_init(void)
{
    int i;

    boot_printk("[vfs] Initializing VFS subsystem...\n");

    /* 初始化各子系统 */
    dcache_init();
    inode_init();
    files_init();

    /* 初始化 init_files（内核态文件描述符表）*/
    for (i = 0; i < NR_OPEN_DEFAULT; i++)
        init_files.fd_array[i] = NULL;
    init_files.next_fd = 0;

    boot_printk("[vfs] VFS initialized\n");
}

/*
 * ============================================================
 * register_filesystem - 注册文件系统类型
 *
 * 将 file_system_type 加入全局链表，使其可被 mount(2) 使用。
 *
 * @fs: 要注册的文件系统类型
 *
 * 返回 0 成功，-1 失败（名称冲突）。
 *
 * 参考：fs/filesystems.c register_filesystem()
 * ============================================================
 */
int register_filesystem(struct file_system_type *fs)
{
    struct file_system_type *p;

    /* 检查是否已注册同名文件系统 */
    for (p = file_systems; p; p = p->next) {
        const char *a = p->name;
        const char *b = fs->name;
        int match = 1;

        while (*a && *b) {
            if (*a++ != *b++) {
                match = 0;
                break;
            }
        }
        if (match && *a == '\0' && *b == '\0') {
            boot_printk("[vfs] ERROR: filesystem already registered: ");
            boot_printk(fs->name);
            boot_printk("\n");
            return -1;
        }
    }

    /* 头插法加入链表 */
    fs->next = file_systems;
    file_systems = fs;

    boot_printk("[vfs] Registered filesystem: ");
    boot_printk(fs->name);
    boot_printk("\n");

    return 0;
}

/*
 * ============================================================
 * get_fs_type - 按名称查找已注册的文件系统类型
 *
 * @name: 文件系统名称（如 "ramfs"、"ext4"）
 *
 * 返回 file_system_type 指针，未找到返回 NULL。
 *
 * 参考：fs/filesystems.c get_fs_type()
 * ============================================================
 */
struct file_system_type *get_fs_type(const char *name)
{
    struct file_system_type *p;

    for (p = file_systems; p; p = p->next) {
        const char *a = p->name;
        const char *b = name;

        while (*a && *b && *a == *b) {
            a++;
            b++;
        }
        if (*a == '\0' && *b == '\0')
            return p;
    }

    return NULL;
}

/*
 * ============================================================
 * alloc_super - 从静态池分配超级块
 *
 * @type: 文件系统类型
 *
 * 返回初始化的 super_block 指针，池满返回 NULL。
 *
 * 参考：fs/super.c alloc_super()
 * ============================================================
 */
struct super_block *alloc_super(struct file_system_type *type)
{
    struct super_block *sb;

    if (sb_pool_idx >= MAX_SUPERBLOCKS) {
        boot_printk("[vfs] ERROR: superblock pool exhausted\n");
        return NULL;
    }

    sb = &sb_pool[sb_pool_idx++];

    sb->s_magic = 0;
    sb->s_blocksize = 4096;
    sb->s_root = NULL;
    sb->s_type = type;
    sb->s_op = NULL;
    sb->s_fs_info = NULL;
    sb->s_next_ino = 1;     /* inode 0 保留 */
    INIT_LIST_HEAD(&sb->s_inodes);

    return sb;
}

/*
 * ============================================================
 * do_mount - 挂载文件系统
 *
 * @dev_name: 块设备名（ramfs 等内存文件系统不需要）
 * @dir_name: 挂载点路径（Phase 7 仅支持 "/"）
 * @type:     文件系统类型名称
 * @flags:    挂载标志（Phase 7 忽略）
 * @data:     文件系统特定数据（Phase 7 忽略）
 *
 * 流程：
 *   1. 查找文件系统类型
 *   2. 调用 fstype->mount() 获取根 dentry
 *   3. 设置 vfsmount
 *
 * 返回 0 成功，负数表示错误。
 *
 * 参考：fs/namespace.c do_mount()
 * ============================================================
 */
int do_mount(const char *dev_name, const char *dir_name,
             const char *type, unsigned long flags, void *data)
{
    struct file_system_type *fstype;
    struct dentry *root;

    boot_printk("[vfs] mount: type=");
    boot_printk(type);
    boot_printk(" on ");
    boot_printk(dir_name);
    boot_printk("\n");

    /* 1. 查找文件系统类型 */
    fstype = get_fs_type(type);
    if (!fstype) {
        boot_printk("[vfs] ERROR: unknown filesystem type: ");
        boot_printk(type);
        boot_printk("\n");
        return -1;
    }

    /* 2. 调用文件系统 mount 回调 */
    root = fstype->mount(fstype, (int)flags, dev_name, data);
    if (!root) {
        boot_printk("[vfs] ERROR: mount() failed for ");
        boot_printk(type);
        boot_printk("\n");
        return -1;
    }

    /* 3. 设置全局根挂载点 */
    root_mnt_storage.mnt_root = root;
    root_mnt_storage.mnt_sb = root->d_sb;
    root_mnt = &root_mnt_storage;

    boot_printk("[vfs] Mounted ");
    boot_printk(type);
    boot_printk(" on ");
    boot_printk(dir_name);
    boot_printk("\n");

    return 0;
}
