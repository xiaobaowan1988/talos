/* SPDX-License-Identifier: GPL-2.0 */
/*
 * arm64os/net/ipv4/ip_input.c
 *
 * IP 收包处理
 *
 * 参考：net/ipv4/ip_input.c
 *
 * Phase 11 实现：
 *   - ip_rcv()：IP 层收包入口
 *   - ip_local_deliver()：本机目的包送往传输层
 *   - 路由判断（本机 / 转发）
 *   - netfilter PRE_ROUTING 和 LOCAL_IN 钩子
 *
 * 简化说明：
 *   - 无 IP 分片重组
 *   - 无 IP 选项处理
 *   - 仅支持本机路由（无转发）
 *   - 无校验和验证（loopback 信任）
 */

#include <linux/types.h>
#include <linux/net.h>
#include <linux/skbuff.h>
#include <linux/netfilter.h>
#include <linux/nsproxy.h>

/* 外部函数声明 */
void boot_printk(const char *s);
void boot_printk_hex(unsigned long val);

/* TCP 层接收（net/ipv4/tcp_input.c 提供）*/
void tcp_v4_rcv(struct sk_buff *skb);

/*
 * ip_local_deliver - 将包送往传输层协议处理函数
 *
 * 参考：net/ipv4/ip_input.c ip_local_deliver()
 *
 * 在 NF_INET_LOCAL_IN 钩子之后调用。
 * 根据 IP 头的 protocol 字段分发到相应传输层。
 */
static void ip_local_deliver_finish(struct sk_buff *skb)
{
    struct iphdr *iph = skb->nh;

    if (!iph) {
        kfree_skb(skb);
        return;
    }

    /* 跳过 IP 头，暴露传输层数据 */
    skb_pull(skb, IP_HDR_LEN);

    switch (iph->protocol) {
    case IPPROTO_TCP:
        tcp_v4_rcv(skb);
        break;
    default:
        /* 不支持的协议，丢弃 */
        kfree_skb(skb);
        break;
    }
}

/*
 * ip_local_deliver - 本机目的包处理
 *
 * 执行 NF_INET_LOCAL_IN 钩子后送往传输层。
 *
 * 参考：net/ipv4/ip_input.c ip_local_deliver()
 */
void ip_local_deliver(struct sk_buff *skb)
{
    unsigned int verdict;

    /* netfilter: NF_INET_LOCAL_IN */
    verdict = nf_hook(&init_net, NF_INET_LOCAL_IN, skb);
    if (verdict != NF_ACCEPT) {
        kfree_skb(skb);
        return;
    }

    ip_local_deliver_finish(skb);
}

/*
 * ip_rcv - IP 层收包入口
 *
 * 参考：net/ipv4/ip_input.c ip_rcv()
 *
 * 处理步骤：
 *   1. 基本头部验证
 *   2. 设置 skb 网络层头部指针
 *   3. 执行 NF_INET_PRE_ROUTING 钩子
 *   4. 路由判断
 *      - 本机目的 → ip_local_deliver()
 *      - 转发 → ip_forward()（本实现不支持）
 */
void ip_rcv(struct sk_buff *skb)
{
    struct iphdr *iph;
    unsigned int verdict;

    if (!skb || skb->len < IP_HDR_LEN) {
        kfree_skb(skb);
        return;
    }

    /* 解析 IP 头 */
    iph = (struct iphdr *)skb->data;
    skb->nh = iph;

    /* 基本验证 */
    if ((iph->version_ihl >> 4) != 4) {
        /* 非 IPv4，丢弃 */
        kfree_skb(skb);
        return;
    }

    /* netfilter: NF_INET_PRE_ROUTING */
    verdict = nf_hook(&init_net, NF_INET_PRE_ROUTING, skb);
    if (verdict != NF_ACCEPT) {
        kfree_skb(skb);
        return;
    }

    /*
     * 路由判断：
     * 本教育内核仅支持本机路由（loopback）。
     * 目的 IP 为 127.0.0.1 或本机地址 → 本地投递。
     */
    ip_local_deliver(skb);
}

/*
 * ip_init - IP 层初始化
 *
 * 参考：net/ipv4/ip_input.c
 */
void ip_init(void)
{
    /* 简化版无需额外初始化 */
}
