/* SPDX-License-Identifier: GPL-2.0 */
/*
 * arm64os/kernel/user_namespace.c
 *
 * User namespace — UID/GID 映射
 *
 * 参考：kernel/user_namespace.c
 *
 * Phase 10 实现：
 *   - init_user_ns：初始用户 namespace（恒等映射）
 *   - create_user_namespace()：创建子 user namespace
 *   - user_ns_set_uid_map()：设置 UID 映射（inner → outer）
 *   - user_ns_map_uid()：将容器内 UID 翻译为宿主 UID
 *
 * User namespace 允许容器内的 root (UID 0) 映射到宿主的非特权 UID。
 * 这是无特权容器（rootless container）的安全基础。
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
static struct user_namespace user_ns_pool[MAX_USER_NS];
static int user_ns_pool_idx = 0;

/*
 * ============================================================
 * 初始 User namespace（恒等映射：inner UID == outer UID）
 * ============================================================
 */
struct user_namespace init_user_ns = {
    .refcount   = 1,
    .parent     = NULL,
    .uid_map    = {{ .inner_start = 0, .outer_start = 0, .count = 65536 }},
    .uid_map_nr = 1,
    .gid_map    = {{ .inner_start = 0, .outer_start = 0, .count = 65536 }},
    .gid_map_nr = 1,
};

/*
 * ============================================================
 * user_ns_init — 初始化 user namespace 子系统
 * ============================================================
 */
void user_ns_init(void)
{
    boot_printk("[user_ns] User namespace initialized (identity mapping)\n");
}

/*
 * ============================================================
 * create_user_namespace — 创建子 user namespace
 *
 * @parent: 父 user namespace
 *
 * 新 namespace 初始无映射（需要通过 set_uid_map 建立映射）。
 *
 * 返回：新的 user_namespace，NULL 表示池耗尽。
 *
 * 参考：kernel/user_namespace.c create_user_ns()
 * ============================================================
 */
struct user_namespace *create_user_namespace(struct user_namespace *parent)
{
    struct user_namespace *ns;

    if (user_ns_pool_idx >= MAX_USER_NS) {
        boot_printk("[user_ns] ERROR: user ns pool exhausted\n");
        return NULL;
    }

    ns = &user_ns_pool[user_ns_pool_idx++];
    ns->refcount = 1;
    ns->parent = parent;
    ns->uid_map_nr = 0;
    ns->gid_map_nr = 0;

    if (parent)
        parent->refcount++;

    return ns;
}

/*
 * ============================================================
 * user_ns_set_uid_map — 设置 UID 映射
 *
 * @ns:    目标 user namespace
 * @inner: 容器内起始 UID
 * @outer: 宿主起始 UID
 * @count: 映射范围（连续 UID 数量）
 *
 * 例：inner=0, outer=1000, count=65536
 *     容器 UID 0-65535 → 宿主 UID 1000-66535
 *
 * 参考：kernel/user_namespace.c map_write()
 * ============================================================
 */
void user_ns_set_uid_map(struct user_namespace *ns,
                          unsigned int inner, unsigned int outer,
                          unsigned int count)
{
    if (!ns || ns->uid_map_nr >= UID_MAP_MAX_ENTRIES)
        return;

    ns->uid_map[ns->uid_map_nr].inner_start = inner;
    ns->uid_map[ns->uid_map_nr].outer_start = outer;
    ns->uid_map[ns->uid_map_nr].count = count;
    ns->uid_map_nr++;
}

/*
 * ============================================================
 * user_ns_map_uid — 将容器内 UID 翻译为宿主 UID
 *
 * @ns:        目标 user namespace
 * @inner_uid: 容器内 UID
 *
 * 返回：宿主 UID，-1 表示无映射。
 *
 * 参考：kernel/user_namespace.c from_kuid()
 * ============================================================
 */
int user_ns_map_uid(struct user_namespace *ns, unsigned int inner_uid)
{
    int i;

    if (!ns)
        return (int)inner_uid;

    for (i = 0; i < ns->uid_map_nr; i++) {
        struct uid_gid_map_entry *e = &ns->uid_map[i];
        if (inner_uid >= e->inner_start &&
            inner_uid < e->inner_start + e->count) {
            return (int)(e->outer_start + (inner_uid - e->inner_start));
        }
    }

    return -1;  /* 无映射 */
}

/*
 * get_user_ns / put_user_ns — 引用计数管理
 */
struct user_namespace *get_user_ns(struct user_namespace *ns)
{
    if (ns)
        ns->refcount++;
    return ns;
}

void put_user_ns(struct user_namespace *ns)
{
    if (ns)
        ns->refcount--;
}
