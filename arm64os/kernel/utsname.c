/* SPDX-License-Identifier: GPL-2.0 */
/*
 * arm64os/kernel/utsname.c
 *
 * UTS namespace — 主机名/域名隔离
 *
 * 参考：kernel/utsname.c
 *
 * Phase 10 实现：
 *   - init_uts_ns：初始 UTS namespace（默认主机名 "arm64os"）
 *   - create_uts_namespace()：创建新 UTS namespace（复制父 namespace）
 *   - uts_ns_set_hostname()：设置主机名
 *
 * UTS namespace 为容器提供独立的主机名/域名视图。
 * 容器可以设置自己的 hostname，不影响宿主或其他容器。
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
static struct uts_namespace uts_ns_pool[MAX_UTS_NS];
static int uts_ns_pool_idx = 0;

/*
 * ============================================================
 * 字符串辅助函数
 * ============================================================
 */
static void ns_strncpy(char *dst, const char *src, int maxlen)
{
    int i;
    for (i = 0; i < maxlen - 1 && src[i]; i++)
        dst[i] = src[i];
    dst[i] = '\0';
}

/*
 * ============================================================
 * 初始 UTS namespace
 * ============================================================
 */
struct uts_namespace init_uts_ns = {
    .count      = 1,
    .nodename   = "arm64os",
    .domainname = "(none)",
};

/*
 * ============================================================
 * uts_ns_init — 初始化 UTS namespace 子系统
 * ============================================================
 */
void uts_ns_init(void)
{
    boot_printk("[uts_ns] UTS namespace initialized (hostname=\"");
    boot_printk(init_uts_ns.nodename);
    boot_printk("\")\n");
}

/*
 * ============================================================
 * create_uts_namespace — 创建新 UTS namespace
 *
 * @orig: 要复制的源 UTS namespace
 *
 * 新 namespace 继承源 namespace 的主机名和域名。
 *
 * 返回：新的 uts_namespace，NULL 表示池耗尽。
 *
 * 参考：kernel/utsname.c clone_uts_ns()
 * ============================================================
 */
struct uts_namespace *create_uts_namespace(struct uts_namespace *orig)
{
    struct uts_namespace *ns;

    if (uts_ns_pool_idx >= MAX_UTS_NS) {
        boot_printk("[uts_ns] ERROR: UTS ns pool exhausted\n");
        return NULL;
    }

    ns = &uts_ns_pool[uts_ns_pool_idx++];
    ns->count = 1;

    /* 复制父 namespace 的主机名和域名 */
    ns_strncpy(ns->nodename, orig->nodename, UTS_NODENAME_LEN);
    ns_strncpy(ns->domainname, orig->domainname, UTS_DOMAINNAME_LEN);

    return ns;
}

/*
 * ============================================================
 * uts_ns_set_hostname — 设置 UTS namespace 的主机名
 *
 * @ns:   目标 UTS namespace
 * @name: 新主机名
 *
 * 参考：kernel/sys.c sethostname()
 * ============================================================
 */
void uts_ns_set_hostname(struct uts_namespace *ns, const char *name)
{
    if (!ns || !name)
        return;
    ns_strncpy(ns->nodename, name, UTS_NODENAME_LEN);
}

/*
 * get_uts_ns / put_uts_ns — 引用计数管理
 */
struct uts_namespace *get_uts_ns(struct uts_namespace *ns)
{
    if (ns)
        ns->count++;
    return ns;
}

void put_uts_ns(struct uts_namespace *ns)
{
    if (ns)
        ns->count--;
}
