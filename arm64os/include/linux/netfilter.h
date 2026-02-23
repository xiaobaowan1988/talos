/* SPDX-License-Identifier: GPL-2.0 */
/*
 * arm64os/include/linux/netfilter.h
 *
 * netfilter 钩子框架
 *
 * 参考：include/linux/netfilter.h
 *       include/uapi/linux/netfilter.h
 *       include/uapi/linux/netfilter_ipv4.h
 *
 * Phase 11 实现：
 *   - 5 个 IPv4 Hook 点（PRE_ROUTING → LOCAL_IN → FORWARD →
 *                         LOCAL_OUT → POST_ROUTING）
 *   - Hook 注册/注销
 *   - Hook 遍历执行（nf_hook_slow）
 *   - 简化 iptables 规则匹配
 *
 * 简化说明：
 *   - 每个 Hook 点最多 8 个钩子
 *   - 无 RCU 保护（单 CPU）
 *   - 无 conntrack
 */

#ifndef __LINUX_NETFILTER_H
#define __LINUX_NETFILTER_H

#include <linux/types.h>

/* 前向声明 */
struct sk_buff;
struct net;

/*
 * ============================================================
 * 协议族（netfilter 层）
 * ============================================================
 */
#define NFPROTO_IPV4        2

/*
 * ============================================================
 * 5 个 IPv4 Hook 点
 *
 * 参考：include/uapi/linux/netfilter_ipv4.h
 *
 * 数据包经过网络栈时依次触发：
 *
 * RX: NIC → PRE_ROUTING → [路由] → LOCAL_IN（本机）
 *                                 → FORWARD（转发）
 * TX: 应用 → LOCAL_OUT → POST_ROUTING → NIC
 * ============================================================
 */
enum nf_inet_hooks {
    NF_INET_PRE_ROUTING  = 0,  /* 收包，路由前 */
    NF_INET_LOCAL_IN     = 1,  /* 收包，目的是本机 */
    NF_INET_FORWARD      = 2,  /* 转发包 */
    NF_INET_LOCAL_OUT    = 3,  /* 本机发出的包 */
    NF_INET_POST_ROUTING = 4,  /* 发包，离开前 */
    NF_INET_NUMHOOKS     = 5,
};

/*
 * ============================================================
 * Hook 返回值
 *
 * 参考：include/uapi/linux/netfilter.h
 * ============================================================
 */
#define NF_DROP     0   /* 丢弃数据包 */
#define NF_ACCEPT   1   /* 继续处理 */
#define NF_STOLEN   2   /* 钩子接管，不再继续 */
#define NF_QUEUE    3   /* 送往用户态队列 */

/*
 * ============================================================
 * Hook 优先级（数值越小越先执行）
 *
 * 参考：include/uapi/linux/netfilter_ipv4.h
 * ============================================================
 */
#define NF_IP_PRI_FIRST         (-400)
#define NF_IP_PRI_CONNTRACK     (-200)
#define NF_IP_PRI_MANGLE        (-150)
#define NF_IP_PRI_NAT_DST       (-100)
#define NF_IP_PRI_FILTER        0
#define NF_IP_PRI_SECURITY      50
#define NF_IP_PRI_NAT_SRC       100
#define NF_IP_PRI_LAST          400

/*
 * ============================================================
 * nf_hook_state — Hook 执行时的上下文
 *
 * 参考：include/linux/netfilter.h struct nf_hook_state
 * ============================================================
 */
struct nf_hook_state {
    unsigned int    hook;       /* 当前 hook 点编号（NF_INET_*）*/
    u8              pf;         /* 协议族（NFPROTO_IPV4）*/
    struct net     *net;        /* 网络命名空间 */
};

/*
 * ============================================================
 * nf_hookfn — Hook 回调函数类型
 *
 * 返回：NF_DROP / NF_ACCEPT / NF_STOLEN / NF_QUEUE
 * ============================================================
 */
typedef unsigned int nf_hookfn(void *priv,
                               struct sk_buff *skb,
                               const struct nf_hook_state *state);

/*
 * ============================================================
 * nf_hook_ops — 注册一个 netfilter 钩子
 *
 * 参考：include/linux/netfilter.h struct nf_hook_ops
 * ============================================================
 */
struct nf_hook_ops {
    nf_hookfn           *hook;      /* 钩子函数 */
    void                *priv;      /* 私有数据 */
    u8                  pf;         /* 协议族（NFPROTO_IPV4）*/
    unsigned int        hooknum;    /* Hook 点编号（NF_INET_*）*/
    int                 priority;   /* 优先级（越小越先执行）*/
};

/*
 * ============================================================
 * nf_hook_entry — Hook 点内的单个钩子条目
 * ============================================================
 */
struct nf_hook_entry {
    nf_hookfn       *hook;
    void            *priv;
    int             priority;
};

/*
 * ============================================================
 * nf_hook_entries — 某个 Hook 点的所有钩子条目数组
 *
 * 参考：include/linux/netfilter.h struct nf_hook_entries
 *
 * 每个 Hook 点维护一个按优先级排序的 entries 数组。
 * 数据包经过时顺序执行所有钩子，直到某个返回非 NF_ACCEPT。
 * ============================================================
 */
#define NF_MAX_HOOKS_PER_POINT  8

struct nf_hook_entries {
    int                     num_hook_entries;
    struct nf_hook_entry    hooks[NF_MAX_HOOKS_PER_POINT];
};

/*
 * ============================================================
 * iptables 简化规则结构
 *
 * 参考：include/uapi/linux/netfilter_ipv4/ip_tables.h
 * ============================================================
 */
#define NFT_MAX_RULES   16

/* 规则匹配条件 */
struct nft_rule {
    __be32          src_ip;         /* 源 IP（0 = 任意）*/
    __be32          src_mask;       /* 源 IP 掩码 */
    __be32          dst_ip;         /* 目的 IP（0 = 任意）*/
    __be32          dst_mask;       /* 目的 IP 掩码 */
    u8              protocol;       /* IP 协议（0 = 任意）*/
    __be16          src_port;       /* 源端口（0 = 任意，网络字节序）*/
    __be16          dst_port;       /* 目的端口（0 = 任意，网络字节序）*/
    int             target;         /* NF_ACCEPT / NF_DROP */
    int             used;           /* 是否有效 */
};

/* 规则链（INPUT / OUTPUT / FORWARD）*/
struct nft_chain {
    struct nft_rule rules[NFT_MAX_RULES];
    int             nr_rules;
    int             policy;         /* 默认策略（NF_ACCEPT / NF_DROP）*/
};

/* 规则表（filter 表）*/
struct nft_table {
    struct nft_chain input;         /* INPUT 链 → NF_INET_LOCAL_IN */
    struct nft_chain output;        /* OUTPUT 链 → NF_INET_LOCAL_OUT */
    struct nft_chain forward;       /* FORWARD 链 → NF_INET_FORWARD */
};

/*
 * ============================================================
 * 外部接口（net/netfilter/core.c 提供）
 * ============================================================
 */

/* netfilter 初始化 */
void nf_init(void);

/* 注册/注销 Hook */
int nf_register_net_hook(struct net *net, const struct nf_hook_ops *ops);
void nf_unregister_net_hook(struct net *net, const struct nf_hook_ops *ops);

/* 执行 Hook 点的所有钩子，返回最终裁决 */
unsigned int nf_hook_slow(struct sk_buff *skb,
                          struct nf_hook_state *state,
                          const struct nf_hook_entries *e);

/* 便捷宏：执行某 Hook 点 */
unsigned int nf_hook(struct net *net, unsigned int hooknum,
                     struct sk_buff *skb);

/*
 * ============================================================
 * 外部接口（net/netfilter/nf_tables_core.c 提供）
 * ============================================================
 */

/* nftables 初始化 */
void nft_init(void);

/* 添加规则到指定链 */
int nft_add_rule(struct nft_chain *chain, const struct nft_rule *rule);

/* 评估规则链，返回裁决 */
unsigned int nft_do_chain(struct nft_chain *chain, struct sk_buff *skb);

/* 全局 filter 表 */
extern struct nft_table nft_filter_table;

#endif /* __LINUX_NETFILTER_H */
