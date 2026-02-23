/* SPDX-License-Identifier: GPL-2.0 */
/*
 * arm64os/security/landlock/net.c
 *
 * Landlock 网络访问控制
 *
 * 参考：security/landlock/net.c
 *
 * Phase 12 实现：
 *   - landlock_socket_connect()：检查 TCP 连接是否被允许
 *
 * 网络访问控制逻辑：
 *   1. 获取当前进程的 Landlock 域
 *   2. 如果未沙箱化或不处理网络访问，直接放行
 *   3. 遍历网络规则，检查端口是否匹配
 *   4. 匹配且允许 → 放行，否则拒绝
 */

#include <linux/types.h>
#include <linux/landlock.h>
#include <linux/sched.h>

/* 外部函数 */
void boot_printk(const char *s);

/* ============================================================
 * landlock_socket_connect - 检查 TCP 连接权限
 *
 * 参数：
 *   port — 目标端口号（主机字节序）
 *
 * 返回值：
 *   0       — 允许
 *   -EACCES — 拒绝
 *
 * 参考：security/landlock/net.c hook_socket_connect()
 * ============================================================ */
int landlock_socket_connect(u16 port)
{
    struct landlock_ruleset *domain;
    u32 i;

    domain = landlock_get_current_domain();

    /* 未沙箱化直接放行 */
    if (!domain)
        return 0;

    /* 不处理网络访问的规则集直接放行 */
    if (!domain->handled_access_net)
        return 0;

    /* 遍历规则查找匹配的端口 */
    for (i = 0; i < LANDLOCK_MAX_RULES; i++) {
        const struct landlock_rule *rule = &domain->rules[i];

        if (!rule->used)
            continue;

        if (rule->type != LANDLOCK_RULE_NET_PORT)
            continue;

        if (rule->port == port) {
            /* 检查是否允许 CONNECT */
            if (rule->allowed_net_access & LANDLOCK_ACCESS_NET_CONNECT_TCP)
                return 0;  /* 允许 */
        }
    }

    /* 没有匹配的规则 → 拒绝 */
    return -13;  /* -EACCES */
}
