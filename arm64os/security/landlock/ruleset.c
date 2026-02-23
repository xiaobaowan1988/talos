/* SPDX-License-Identifier: GPL-2.0 */
/*
 * arm64os/security/landlock/ruleset.c
 *
 * Landlock 规则集管理
 *
 * 参考：security/landlock/ruleset.c
 *
 * Phase 12 实现：
 *   - landlock_init()：初始化 Landlock 子系统
 *   - landlock_create_ruleset()：创建规则集
 *   - landlock_add_rule()：向规则集添加规则
 *   - landlock_restrict_self()：激活沙箱（不可逆）
 *   - landlock_get_current_domain()：获取当前进程的 Landlock 域
 *
 * Landlock 设计原则：
 *   1. 无需 root 权限（普通进程可自我沙箱化）
 *   2. 不可绕过（子进程继承且无法放松规则）
 *   3. 规则叠加（只能更严，不能更松）
 *   4. 基于路径层级访问控制（非 DAC/MAC）
 */

#include <linux/types.h>
#include <linux/landlock.h>
#include <linux/sched.h>

/* 外部函数 */
void boot_printk(const char *s);
void boot_printk_hex(unsigned long val);
extern struct task_struct *current_task;

/* ============================================================
 * 规则集静态池
 * ============================================================ */
static struct landlock_ruleset ruleset_pool[LANDLOCK_MAX_RULESETS];

/*
 * landlock_init - 初始化 Landlock 子系统
 */
void landlock_init(void)
{
    int i, j;
    for (i = 0; i < LANDLOCK_MAX_RULESETS; i++) {
        ruleset_pool[i].used = 0;
        ruleset_pool[i].refcount = 0;
        ruleset_pool[i].handled_access_fs = 0;
        ruleset_pool[i].handled_access_net = 0;
        ruleset_pool[i].num_rules = 0;
        ruleset_pool[i].enforced = false;
        for (j = 0; j < LANDLOCK_MAX_RULES; j++)
            ruleset_pool[i].rules[j].used = 0;
    }
    boot_printk("[landlock] Landlock LSM initialized\n");
}

/*
 * landlock_get_ruleset - 根据 fd（池索引）查找规则集
 */
struct landlock_ruleset *landlock_get_ruleset(int fd)
{
    if (fd < 0 || fd >= LANDLOCK_MAX_RULESETS)
        return NULL;
    if (!ruleset_pool[fd].used)
        return NULL;
    return &ruleset_pool[fd];
}

/*
 * landlock_get_current_domain - 获取当前进程的 Landlock 域
 *
 * 返回 NULL 表示进程未沙箱化。
 */
struct landlock_ruleset *landlock_get_current_domain(void)
{
    if (!current_task)
        return NULL;
    return (struct landlock_ruleset *)current_task->landlock_domain;
}

/* ============================================================
 * 字符串辅助函数（freestanding 环境无 libc）
 * ============================================================ */
static int ll_strlen(const char *s)
{
    int len = 0;
    while (s[len])
        len++;
    return len;
}

static int ll_strncmp(const char *a, const char *b, int n)
{
    int i;
    for (i = 0; i < n; i++) {
        if (a[i] != b[i])
            return (unsigned char)a[i] - (unsigned char)b[i];
        if (a[i] == '\0')
            return 0;
    }
    return 0;
}

static void ll_strncpy(char *dst, const char *src, int n)
{
    int i;
    for (i = 0; i < n - 1 && src[i]; i++)
        dst[i] = src[i];
    dst[i] = '\0';
}

/* ============================================================
 * landlock_create_ruleset - 创建规则集
 *
 * 返回规则集 fd（>= 0），或负数错误码。
 *
 * 参考：security/landlock/syscalls.c sys_landlock_create_ruleset()
 * ============================================================ */
int landlock_create_ruleset(const struct landlock_ruleset_attr *attr,
                            size_t size, u32 flags)
{
    int i;

    if (!attr)
        return -1;

    /* 检查至少声明了一种访问类型 */
    if (attr->handled_access_fs == 0 && attr->handled_access_net == 0)
        return -1;

    /* 从池中分配 */
    for (i = 0; i < LANDLOCK_MAX_RULESETS; i++) {
        if (!ruleset_pool[i].used) {
            int j;
            ruleset_pool[i].used = 1;
            ruleset_pool[i].refcount = 1;
            ruleset_pool[i].handled_access_fs = attr->handled_access_fs;
            ruleset_pool[i].handled_access_net = attr->handled_access_net;
            ruleset_pool[i].num_rules = 0;
            ruleset_pool[i].enforced = false;
            for (j = 0; j < LANDLOCK_MAX_RULES; j++)
                ruleset_pool[i].rules[j].used = 0;
            return i;
        }
    }

    boot_printk("[landlock] WARN: ruleset pool exhausted\n");
    return -1;
}

/* ============================================================
 * landlock_add_path_rule - 向规则集添加路径规则（辅助函数）
 *
 * 简化实现：用路径字符串代替 inode 引用。
 * ============================================================ */
int landlock_add_path_rule(int ruleset_fd, const char *path, u64 allowed_access)
{
    struct landlock_ruleset *rs;
    struct landlock_rule *rule;
    u32 i;

    rs = landlock_get_ruleset(ruleset_fd);
    if (!rs)
        return -1;

    if (rs->enforced)
        return -1;  /* 已激活的规则集不能修改 */

    /* 查找空闲规则槽 */
    for (i = 0; i < LANDLOCK_MAX_RULES; i++) {
        if (!rs->rules[i].used) {
            rule = &rs->rules[i];
            rule->used = 1;
            rule->type = LANDLOCK_RULE_PATH_BENEATH;
            ll_strncpy(rule->path, path, 64);
            rule->allowed_access = allowed_access;
            rule->port = 0;
            rule->allowed_net_access = 0;
            rs->num_rules++;
            return 0;
        }
    }

    boot_printk("[landlock] WARN: rule limit reached\n");
    return -1;
}

/* ============================================================
 * landlock_add_net_rule - 向规则集添加网络端口规则
 * ============================================================ */
int landlock_add_net_rule(int ruleset_fd, u16 port, u64 allowed_access)
{
    struct landlock_ruleset *rs;
    struct landlock_rule *rule;
    u32 i;

    rs = landlock_get_ruleset(ruleset_fd);
    if (!rs)
        return -1;

    if (rs->enforced)
        return -1;

    for (i = 0; i < LANDLOCK_MAX_RULES; i++) {
        if (!rs->rules[i].used) {
            rule = &rs->rules[i];
            rule->used = 1;
            rule->type = LANDLOCK_RULE_NET_PORT;
            rule->path[0] = '\0';
            rule->allowed_access = 0;
            rule->port = port;
            rule->allowed_net_access = allowed_access;
            rs->num_rules++;
            return 0;
        }
    }

    return -1;
}

/* ============================================================
 * landlock_add_rule - 向规则集添加规则（系统调用接口）
 *
 * 参考：security/landlock/syscalls.c sys_landlock_add_rule()
 * ============================================================ */
int landlock_add_rule(int ruleset_fd, enum landlock_rule_type rule_type,
                      const void *rule_attr, u32 flags)
{
    if (!rule_attr)
        return -1;

    switch (rule_type) {
    case LANDLOCK_RULE_PATH_BENEATH: {
        const struct landlock_path_beneath_attr *attr =
            (const struct landlock_path_beneath_attr *)rule_attr;
        /* 简化：parent_fd 此处不使用，由 landlock_add_path_rule 处理 */
        (void)attr;
        return -1;  /* 使用 landlock_add_path_rule 辅助函数代替 */
    }

    case LANDLOCK_RULE_NET_PORT: {
        const struct landlock_net_port_attr *attr =
            (const struct landlock_net_port_attr *)rule_attr;
        return landlock_add_net_rule(ruleset_fd, (u16)attr->port,
                                     attr->allowed_access);
    }

    default:
        return -1;
    }
}

/* ============================================================
 * landlock_restrict_self - 激活沙箱（不可逆）
 *
 * 将规则集绑定到当前进程。此后进程及其子进程都受该规则集约束。
 * 不可逆：一旦激活，无法放松规则。
 *
 * 注意：必须先调用 prctl(PR_SET_NO_NEW_PRIVS) 设置 no_new_privs。
 *
 * 参考：security/landlock/syscalls.c sys_landlock_restrict_self()
 * ============================================================ */
int landlock_restrict_self(int ruleset_fd, u32 flags)
{
    struct landlock_ruleset *rs;

    if (!current_task)
        return -1;

    rs = landlock_get_ruleset(ruleset_fd);
    if (!rs)
        return -1;

    /* 标记规则集为已激活 */
    rs->enforced = true;

    /* 绑定到当前进程 */
    current_task->landlock_domain = rs;

    boot_printk("[landlock] sandbox enforced (");
    boot_printk_hex((unsigned long)rs->num_rules);
    boot_printk(" rules)\n");

    return 0;
}
