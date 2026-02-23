/* SPDX-License-Identifier: GPL-2.0 */
/*
 * arm64os/include/linux/landlock.h
 *
 * Landlock LSM（Linux Security Module）— 不可权限提升的沙箱机制
 *
 * 参考：include/uapi/linux/landlock.h
 *       security/landlock/ruleset.h
 *       security/landlock/fs.h
 *
 * Phase 12 实现：
 *   - Landlock 规则集（ruleset）：声明要限制的访问类型
 *   - Landlock 规则（rule）：路径 → 允许的权限
 *   - 文件系统访问控制（hook_file_open）
 *   - 网络访问控制（hook_socket_connect）
 *   - 三个系统调用：create_ruleset, add_rule, restrict_self
 *
 * 设计原则：
 *   1. 无需 root 权限（普通进程可自我沙箱化）
 *   2. 不可绕过（子进程继承且无法放松规则）
 *   3. 规则叠加（只能更严，不能更松）
 *   4. 基于路径层级访问控制
 */

#ifndef __LINUX_LANDLOCK_H
#define __LINUX_LANDLOCK_H

#include <linux/types.h>

/* ============================================================
 * Landlock 文件系统访问权限位
 *
 * 参考：include/uapi/linux/landlock.h
 * ============================================================ */
#define LANDLOCK_ACCESS_FS_EXECUTE          (1ULL << 0)
#define LANDLOCK_ACCESS_FS_WRITE_FILE       (1ULL << 1)
#define LANDLOCK_ACCESS_FS_READ_FILE        (1ULL << 2)
#define LANDLOCK_ACCESS_FS_READ_DIR         (1ULL << 3)
#define LANDLOCK_ACCESS_FS_REMOVE_DIR       (1ULL << 4)
#define LANDLOCK_ACCESS_FS_REMOVE_FILE      (1ULL << 5)
#define LANDLOCK_ACCESS_FS_MAKE_CHAR        (1ULL << 6)
#define LANDLOCK_ACCESS_FS_MAKE_DIR         (1ULL << 7)
#define LANDLOCK_ACCESS_FS_MAKE_REG         (1ULL << 8)
#define LANDLOCK_ACCESS_FS_MAKE_SOCK        (1ULL << 9)
#define LANDLOCK_ACCESS_FS_MAKE_FIFO        (1ULL << 10)
#define LANDLOCK_ACCESS_FS_MAKE_BLOCK       (1ULL << 11)
#define LANDLOCK_ACCESS_FS_MAKE_SYM         (1ULL << 12)
#define LANDLOCK_ACCESS_FS_REFER            (1ULL << 13)
#define LANDLOCK_ACCESS_FS_TRUNCATE         (1ULL << 14)

/* 所有文件系统访问权限的联合 */
#define LANDLOCK_ACCESS_FS_ALL  ( \
    LANDLOCK_ACCESS_FS_EXECUTE      | \
    LANDLOCK_ACCESS_FS_WRITE_FILE   | \
    LANDLOCK_ACCESS_FS_READ_FILE    | \
    LANDLOCK_ACCESS_FS_READ_DIR     | \
    LANDLOCK_ACCESS_FS_REMOVE_DIR   | \
    LANDLOCK_ACCESS_FS_REMOVE_FILE  | \
    LANDLOCK_ACCESS_FS_MAKE_CHAR    | \
    LANDLOCK_ACCESS_FS_MAKE_DIR     | \
    LANDLOCK_ACCESS_FS_MAKE_REG     | \
    LANDLOCK_ACCESS_FS_MAKE_SOCK    | \
    LANDLOCK_ACCESS_FS_MAKE_FIFO    | \
    LANDLOCK_ACCESS_FS_MAKE_BLOCK   | \
    LANDLOCK_ACCESS_FS_MAKE_SYM     | \
    LANDLOCK_ACCESS_FS_REFER        | \
    LANDLOCK_ACCESS_FS_TRUNCATE     )

/* ============================================================
 * Landlock 网络访问权限位（ABI v4+）
 * ============================================================ */
#define LANDLOCK_ACCESS_NET_BIND_TCP        (1ULL << 0)
#define LANDLOCK_ACCESS_NET_CONNECT_TCP     (1ULL << 1)

#define LANDLOCK_ACCESS_NET_ALL ( \
    LANDLOCK_ACCESS_NET_BIND_TCP    | \
    LANDLOCK_ACCESS_NET_CONNECT_TCP )

/* ============================================================
 * Landlock 规则类型
 * ============================================================ */
enum landlock_rule_type {
    LANDLOCK_RULE_PATH_BENEATH = 1,     /* 路径层级规则 */
    LANDLOCK_RULE_NET_PORT     = 2,     /* 网络端口规则 */
};

/* ============================================================
 * Landlock 用户态接口结构体
 * ============================================================ */

/* landlock_create_ruleset() 属性 */
struct landlock_ruleset_attr {
    u64     handled_access_fs;      /* 要限制的文件系统访问类型 */
    u64     handled_access_net;     /* 要限制的网络访问类型 */
};

/* landlock_add_rule() 的路径规则属性 */
struct landlock_path_beneath_attr {
    u64     allowed_access;         /* 允许的访问权限 */
    int     parent_fd;              /* 父目录的 fd（简化：使用路径名索引）*/
};

/* landlock_add_rule() 的网络规则属性 */
struct landlock_net_port_attr {
    u64     allowed_access;         /* 允许的网络权限 */
    u64     port;                   /* 端口号 */
};

/* ============================================================
 * Landlock 内核数据结构
 * ============================================================ */

/* 最大规则数 */
#define LANDLOCK_MAX_RULES      16

/* 最大规则集数 */
#define LANDLOCK_MAX_RULESETS    8

/* 单条规则（路径 → 允许的权限）*/
struct landlock_rule {
    int             used;               /* 是否已分配 */
    enum landlock_rule_type type;       /* 规则类型 */

    /* 路径规则（LANDLOCK_RULE_PATH_BENEATH）*/
    char            path[64];           /* 路径前缀（简化：存储路径字符串）*/
    u64             allowed_access;     /* 允许的访问权限 */

    /* 网络规则（LANDLOCK_RULE_NET_PORT）*/
    u16             port;               /* 端口号 */
    u64             allowed_net_access; /* 允许的网络权限 */
};

/* 规则集 */
struct landlock_ruleset {
    int                     used;           /* 是否已分配 */
    int                     refcount;       /* 引用计数 */
    u64                     handled_access_fs;   /* 限制的文件系统访问类型 */
    u64                     handled_access_net;  /* 限制的网络访问类型 */
    u32                     num_rules;      /* 规则数 */
    struct landlock_rule    rules[LANDLOCK_MAX_RULES]; /* 规则数组 */
    bool                    enforced;       /* 是否已激活（restrict_self 后为 true）*/
};

/* ============================================================
 * Landlock 接口函数
 * ============================================================ */

/* 初始化 Landlock 子系统 */
void landlock_init(void);

/* 系统调用：创建规则集 */
int landlock_create_ruleset(const struct landlock_ruleset_attr *attr,
                            size_t size, u32 flags);

/* 系统调用：向规则集添加规则 */
int landlock_add_rule(int ruleset_fd, enum landlock_rule_type rule_type,
                      const void *rule_attr, u32 flags);

/* 系统调用：激活沙箱（不可逆）*/
int landlock_restrict_self(int ruleset_fd, u32 flags);

/* 规则集管理 */
struct landlock_ruleset *landlock_get_ruleset(int fd);

/* LSM 钩子：检查文件访问 */
int landlock_file_open(const char *pathname, u64 access_request);

/* LSM 钩子：检查网络访问 */
int landlock_socket_connect(u16 port);

/* 获取当前进程的 Landlock 域 */
struct landlock_ruleset *landlock_get_current_domain(void);

/* 路径辅助：添加路径规则 */
int landlock_add_path_rule(int ruleset_fd, const char *path, u64 allowed_access);

/* 网络辅助：添加端口规则 */
int landlock_add_net_rule(int ruleset_fd, u16 port, u64 allowed_access);

#endif /* __LINUX_LANDLOCK_H */
