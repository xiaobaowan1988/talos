/* SPDX-License-Identifier: GPL-2.0 */
/*
 * arm64os/security/landlock/fs.c
 *
 * Landlock 文件系统访问控制
 *
 * 参考：security/landlock/fs.c
 *
 * Phase 12 实现：
 *   - landlock_file_open()：在文件打开时检查 Landlock 权限
 *   - check_access_path()：遍历路径层级，检查每个分量的规则
 *
 * 访问控制逻辑：
 *   1. 获取当前进程的 Landlock 域（规则集）
 *   2. 如果未沙箱化（domain == NULL），直接放行
 *   3. 检查请求的访问类型是否在 handled_access_fs 中
 *   4. 遍历规则，检查文件路径是否匹配某条规则
 *   5. 匹配则放行，不匹配则拒绝（EACCES）
 */

#include <linux/types.h>
#include <linux/landlock.h>
#include <linux/sched.h>

/* 外部函数 */
void boot_printk(const char *s);

/* ============================================================
 * 字符串辅助函数
 * ============================================================ */
static int fs_strlen(const char *s)
{
    int len = 0;
    while (s[len])
        len++;
    return len;
}

/*
 * path_is_beneath - 检查 pathname 是否在 rule_path 之下
 *
 * 例如：
 *   rule_path = "/etc"
 *   pathname  = "/etc/hostname"  → true
 *   pathname  = "/etc"           → true
 *   pathname  = "/etcetera"      → false
 *   pathname  = "/home/user"     → false
 */
static bool path_is_beneath(const char *pathname, const char *rule_path)
{
    int rule_len = fs_strlen(rule_path);
    int path_len = fs_strlen(pathname);
    int i;

    /* 规则路径不能比文件路径长 */
    if (rule_len > path_len)
        return false;

    /* 前缀匹配 */
    for (i = 0; i < rule_len; i++) {
        if (pathname[i] != rule_path[i])
            return false;
    }

    /* 完整匹配，或者文件路径在规则路径后有 '/' 分隔符 */
    if (path_len == rule_len)
        return true;    /* 完全匹配 */

    if (pathname[rule_len] == '/')
        return true;    /* 子目录匹配 */

    /* 特殊情况：规则路径为 "/" */
    if (rule_len == 1 && rule_path[0] == '/')
        return true;

    return false;
}

/* ============================================================
 * landlock_file_open - LSM 钩子：检查文件访问权限
 *
 * 在文件打开时调用，检查当前进程的 Landlock 规则集
 * 是否允许访问指定路径。
 *
 * 参数：
 *   pathname       — 文件路径
 *   access_request — 请求的访问类型（LANDLOCK_ACCESS_FS_*）
 *
 * 返回值：
 *   0      — 允许
 *   -EACCES — 拒绝
 *
 * 参考：security/landlock/fs.c hook_file_open()
 * ============================================================ */
int landlock_file_open(const char *pathname, u64 access_request)
{
    struct landlock_ruleset *domain;
    u32 i;

    /* 获取当前进程的 Landlock 域 */
    domain = landlock_get_current_domain();

    /* 未沙箱化进程直接放行 */
    if (!domain)
        return 0;

    /* 检查请求的访问类型是否在受控范围内 */
    if (!(access_request & domain->handled_access_fs))
        return 0;  /* 不在限制范围内的访问类型直接放行 */

    /* 遍历规则，查找匹配的路径 */
    for (i = 0; i < LANDLOCK_MAX_RULES; i++) {
        const struct landlock_rule *rule = &domain->rules[i];

        if (!rule->used)
            continue;

        if (rule->type != LANDLOCK_RULE_PATH_BENEATH)
            continue;

        /* 检查路径是否在规则路径之下 */
        if (path_is_beneath(pathname, rule->path)) {
            /* 检查规则是否允许请求的访问类型 */
            if ((rule->allowed_access & access_request) == access_request)
                return 0;  /* 允许 */
        }
    }

    /* 没有匹配的规则 → 拒绝 */
    return -13;  /* -EACCES */
}
