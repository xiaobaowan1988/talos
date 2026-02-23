/* SPDX-License-Identifier: GPL-2.0 */
/*
 * arm64os/net/netfilter/nf_tables_core.c
 *
 * nftables 规则引擎 — 简化的 iptables 规则匹配
 *
 * 参考：net/netfilter/nf_tables_core.c
 *       net/ipv4/netfilter/iptable_filter.c
 *
 * Phase 11 实现：
 *   - nft_init()：初始化 filter 表（INPUT/OUTPUT/FORWARD 链）
 *   - nft_add_rule()：添加规则到链
 *   - nft_do_chain()：遍历链中的规则，匹配并执行 target
 *
 * 简化说明：
 *   - 仅 filter 表
 *   - 三条内置链：INPUT、OUTPUT、FORWARD
 *   - 匹配条件：源/目的 IP、协议、源/目的端口
 *   - target：ACCEPT / DROP
 *   - 默认策略：ACCEPT
 */

#include <linux/types.h>
#include <linux/net.h>
#include <linux/netfilter.h>
#include <linux/skbuff.h>

/* 外部函数声明 */
void boot_printk(const char *s);

/* ---- 全局 filter 表 ---- */
struct nft_table nft_filter_table;

/*
 * nft_init - 初始化 nftables 规则引擎
 *
 * 参考：net/netfilter/nf_tables_api.c
 */
void nft_init(void)
{
    int i;

    /* 初始化 INPUT 链 */
    nft_filter_table.input.nr_rules = 0;
    nft_filter_table.input.policy = NF_ACCEPT;
    for (i = 0; i < NFT_MAX_RULES; i++)
        nft_filter_table.input.rules[i].used = 0;

    /* 初始化 OUTPUT 链 */
    nft_filter_table.output.nr_rules = 0;
    nft_filter_table.output.policy = NF_ACCEPT;
    for (i = 0; i < NFT_MAX_RULES; i++)
        nft_filter_table.output.rules[i].used = 0;

    /* 初始化 FORWARD 链 */
    nft_filter_table.forward.nr_rules = 0;
    nft_filter_table.forward.policy = NF_ACCEPT;
    for (i = 0; i < NFT_MAX_RULES; i++)
        nft_filter_table.forward.rules[i].used = 0;

    boot_printk("[net] nftables rule engine initialized\n");
}

/*
 * nft_add_rule - 向链中添加一条规则
 *
 * 参考：net/netfilter/nf_tables_api.c nf_tables_newrule()
 *
 * @chain: 目标链（INPUT/OUTPUT/FORWARD）
 * @rule:  规则内容（匹配条件 + target）
 *
 * 返回：0 成功，-1 失败
 */
int nft_add_rule(struct nft_chain *chain, const struct nft_rule *rule)
{
    int i;

    if (!chain || !rule)
        return -1;

    if (chain->nr_rules >= NFT_MAX_RULES) {
        boot_printk("[nft] WARN: chain full\n");
        return -1;
    }

    /* 找到一个空闲槽位 */
    for (i = 0; i < NFT_MAX_RULES; i++) {
        if (!chain->rules[i].used) {
            /* 逐字段复制（freestanding 环境无 memcpy） */
            chain->rules[i].src_ip   = rule->src_ip;
            chain->rules[i].src_mask = rule->src_mask;
            chain->rules[i].dst_ip   = rule->dst_ip;
            chain->rules[i].dst_mask = rule->dst_mask;
            chain->rules[i].protocol = rule->protocol;
            chain->rules[i].src_port = rule->src_port;
            chain->rules[i].dst_port = rule->dst_port;
            chain->rules[i].target   = rule->target;
            chain->rules[i].used = 1;
            chain->nr_rules++;
            return 0;
        }
    }

    return -1;
}

/*
 * nft_match_rule - 检查 skb 是否匹配某条规则
 *
 * 参考：net/ipv4/netfilter/iptable_filter.c ipt_match_ip()
 *
 * 匹配条件（所有非零条件必须都满足）：
 *   - src_ip/src_mask：源 IP & mask == rule.src_ip & mask
 *   - dst_ip/dst_mask：目的 IP & mask == rule.dst_ip & mask
 *   - protocol：IP 协议号
 *   - src_port：源端口
 *   - dst_port：目的端口
 *
 * 返回：1 匹配，0 不匹配
 */
static int nft_match_rule(const struct nft_rule *rule, struct sk_buff *skb)
{
    struct iphdr *iph;
    struct tcphdr *th;

    if (!rule || !skb)
        return 0;

    iph = skb->nh;
    if (!iph)
        return 0;

    /* 匹配源 IP */
    if (rule->src_ip != 0) {
        if ((iph->saddr & rule->src_mask) !=
            (rule->src_ip & rule->src_mask))
            return 0;
    }

    /* 匹配目的 IP */
    if (rule->dst_ip != 0) {
        if ((iph->daddr & rule->dst_mask) !=
            (rule->dst_ip & rule->dst_mask))
            return 0;
    }

    /* 匹配协议 */
    if (rule->protocol != 0) {
        if (iph->protocol != rule->protocol)
            return 0;
    }

    /* 匹配端口（仅 TCP/UDP 有效）*/
    th = skb->th;
    if (th) {
        if (rule->src_port != 0 && th->source != rule->src_port)
            return 0;
        if (rule->dst_port != 0 && th->dest != rule->dst_port)
            return 0;
    } else {
        /* 无传输层头但规则要求匹配端口 → 不匹配 */
        if (rule->src_port != 0 || rule->dst_port != 0)
            return 0;
    }

    return 1;   /* 所有条件满足 */
}

/*
 * nft_do_chain - 遍历规则链，评估数据包
 *
 * 参考：net/netfilter/nf_tables_core.c nft_do_chain()
 *       net/ipv4/netfilter/iptable_filter.c ipt_do_table()
 *
 * 从第一条规则开始顺序匹配：
 *   - 如果匹配，执行 target（ACCEPT/DROP）
 *   - 如果不匹配，继续下一条
 *   - 所有规则都不匹配 → 执行链的默认策略
 *
 * @chain: 规则链
 * @skb:   待评估的数据包
 *
 * 返回：NF_ACCEPT / NF_DROP
 */
unsigned int nft_do_chain(struct nft_chain *chain, struct sk_buff *skb)
{
    int i;

    if (!chain)
        return NF_ACCEPT;

    for (i = 0; i < NFT_MAX_RULES; i++) {
        if (!chain->rules[i].used)
            continue;

        if (nft_match_rule(&chain->rules[i], skb))
            return (unsigned int)chain->rules[i].target;
    }

    /* 无规则匹配，返回默认策略 */
    return (unsigned int)chain->policy;
}
