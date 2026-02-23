/* SPDX-License-Identifier: GPL-2.0 */
/*
 * arm64os/fs/mount.c
 *
 * Mount namespace — 文件系统挂载点隔离
 *
 * 参考：fs/namespace.c
 *       fs/mount.h
 *
 * Phase 10 实现：
 *   - init_mnt_ns：初始 mount namespace
 *   - create_mnt_namespace()：创建新 mount namespace（复制父 namespace 挂载表）
 *   - mnt_ns_add_mount()：向 namespace 的挂载表添加条目
 *   - mnt_ns_has_mount()：检查 namespace 中是否存在指定挂载点
 *
 * Mount namespace 为容器提供独立的挂载点视图。
 * clone(CLONE_NEWNS) 创建新的 mount namespace，
 * 新 namespace 是父 namespace 挂载表的副本。
 * 之后的 mount/umount 操作只影响当前 namespace。
 *
 * 容器典型流程：
 *   1. unshare(CLONE_NEWNS) — 创建新 mount namespace
 *   2. mount overlayfs — 挂载容器根文件系统
 *   3. pivot_root — 切换根目录到容器文件系统
 *   4. umount 旧根 — 清理宿主挂载点
 */

#include <linux/types.h>
#include <linux/nsproxy.h>

/* 外部函数 */
void boot_printk(const char *s);
void boot_printk_hex(unsigned long val);

/*
 * ============================================================
 * 静态池
 * ============================================================
 */
static struct mnt_namespace mnt_ns_pool[MAX_MNT_NS];
static int mnt_ns_pool_idx = 0;

/*
 * ============================================================
 * 字符串辅助函数
 * ============================================================
 */
static void mnt_strncpy(char *dst, const char *src, int maxlen)
{
    int i;
    for (i = 0; i < maxlen - 1 && src[i]; i++)
        dst[i] = src[i];
    dst[i] = '\0';
}

static int mnt_strlen(const char *s)
{
    int i = 0;
    while (s[i])
        i++;
    return i;
}

static int mnt_strcmp(const char *a, const char *b)
{
    while (*a && *b && *a == *b) {
        a++;
        b++;
    }
    return (unsigned char)*a - (unsigned char)*b;
}

/*
 * ============================================================
 * 初始 mount namespace
 * ============================================================
 */
struct mnt_namespace init_mnt_ns = {
    .count      = 1,
    .nr_mounts  = 0,
};

/*
 * ============================================================
 * mnt_ns_init — 初始化 mount namespace 子系统
 * ============================================================
 */
void mnt_ns_init(void)
{
    int i;

    /* 清零初始挂载表 */
    for (i = 0; i < MNT_NS_MAX_MOUNTS; i++)
        init_mnt_ns.mounts[i].used = 0;

    boot_printk("[mnt_ns] Mount namespace initialized\n");
}

/*
 * ============================================================
 * create_mnt_namespace — 创建新 mount namespace
 *
 * @orig: 要复制的源 mount namespace
 *
 * 新 namespace 继承源 namespace 的完整挂载表副本。
 *
 * 返回：新的 mnt_namespace，NULL 表示池耗尽。
 *
 * 参考：fs/namespace.c copy_mnt_ns()
 * ============================================================
 */
struct mnt_namespace *create_mnt_namespace(struct mnt_namespace *orig)
{
    struct mnt_namespace *ns;
    int i;

    if (mnt_ns_pool_idx >= MAX_MNT_NS) {
        boot_printk("[mnt_ns] ERROR: mount ns pool exhausted\n");
        return NULL;
    }

    ns = &mnt_ns_pool[mnt_ns_pool_idx++];
    ns->count = 1;
    ns->nr_mounts = orig->nr_mounts;

    /* 复制父 namespace 的挂载表 */
    for (i = 0; i < MNT_NS_MAX_MOUNTS; i++) {
        if (orig->mounts[i].used) {
            mnt_strncpy(ns->mounts[i].path, orig->mounts[i].path,
                         MNT_NS_MAX_PATH);
            ns->mounts[i].pathlen = orig->mounts[i].pathlen;
            mnt_strncpy(ns->mounts[i].fstype, orig->mounts[i].fstype, 16);
            ns->mounts[i].used = 1;
        } else {
            ns->mounts[i].used = 0;
        }
    }

    return ns;
}

/*
 * ============================================================
 * mnt_ns_add_mount — 向 namespace 的挂载表添加条目
 *
 * @ns:     目标 mount namespace
 * @path:   挂载路径（如 "/sq"）
 * @fstype: 文件系统类型名（如 "squashfs"）
 *
 * 返回 0 成功，-1 表示挂载表已满。
 * ============================================================
 */
int mnt_ns_add_mount(struct mnt_namespace *ns, const char *path,
                      const char *fstype)
{
    int i;

    if (!ns || !path)
        return -1;

    for (i = 0; i < MNT_NS_MAX_MOUNTS; i++) {
        if (!ns->mounts[i].used) {
            mnt_strncpy(ns->mounts[i].path, path, MNT_NS_MAX_PATH);
            ns->mounts[i].pathlen = mnt_strlen(path);
            if (fstype)
                mnt_strncpy(ns->mounts[i].fstype, fstype, 16);
            else
                ns->mounts[i].fstype[0] = '\0';
            ns->mounts[i].used = 1;
            ns->nr_mounts++;
            return 0;
        }
    }

    return -1;  /* 挂载表已满 */
}

/*
 * ============================================================
 * mnt_ns_has_mount — 检查 namespace 中是否存在指定挂载点
 *
 * @ns:   目标 mount namespace
 * @path: 挂载路径
 *
 * 返回 1 存在，0 不存在。
 * ============================================================
 */
int mnt_ns_has_mount(struct mnt_namespace *ns, const char *path)
{
    int i;

    if (!ns || !path)
        return 0;

    for (i = 0; i < MNT_NS_MAX_MOUNTS; i++) {
        if (ns->mounts[i].used &&
            mnt_strcmp(ns->mounts[i].path, path) == 0) {
            return 1;
        }
    }

    return 0;
}

/*
 * get_mnt_ns / put_mnt_ns — 引用计数管理
 */
struct mnt_namespace *get_mnt_ns(struct mnt_namespace *ns)
{
    if (ns)
        ns->count++;
    return ns;
}

void put_mnt_ns(struct mnt_namespace *ns)
{
    if (ns)
        ns->count--;
}
