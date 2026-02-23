/* SPDX-License-Identifier: GPL-2.0 */
/*
 * arm64os/net/ipv4/ip_output.c
 *
 * IP 发包处理 + loopback 设备
 *
 * 参考：net/ipv4/ip_output.c
 *       drivers/net/loopback.c
 *
 * Phase 11 实现：
 *   - ip_queue_xmit()：TCP 发包入口
 *   - ip_build_and_send()：构造 IP 头 + 发送
 *   - loopback 设备：同步将发送包送回接收路径
 *   - netfilter LOCAL_OUT 和 POST_ROUTING 钩子
 *   - loopback 接收队列：避免同步递归
 *
 * 简化说明：
 *   - 仅 loopback 发送（无真实 NIC 发送）
 *   - 无 IP 分片
 *   - 无路由表（所有包走 loopback）
 *   - 使用队列避免递归调用
 */

#include <linux/types.h>
#include <linux/net.h>
#include <linux/skbuff.h>
#include <linux/netfilter.h>
#include <linux/nsproxy.h>

/* 外部函数声明 */
void boot_printk(const char *s);
void boot_printk_hex(unsigned long val);

/* IP 收包入口（net/ipv4/ip_input.c 提供）*/
void ip_rcv(struct sk_buff *skb);

/*
 * ============================================================
 * Loopback 接收队列
 *
 * 为避免 ip_queue_xmit → loopback → ip_rcv → tcp_v4_rcv →
 * tcp_send_synack → ip_queue_xmit 的递归调用，
 * loopback 设备将发送的包放入队列，由 net_rx_process() 扁平化处理。
 *
 * 参考：net/core/dev.c softnet_data (NAPI poll queue)
 * ============================================================
 */
#define LOOPBACK_QUEUE_SIZE     64

static struct sk_buff *loopback_queue[LOOPBACK_QUEUE_SIZE];
static int loopback_head;
static int loopback_tail;

/* IP 标识号递增计数器 */
static u16 ip_id_counter;

/*
 * loopback_xmit - Loopback 发送函数
 *
 * 将包加入 loopback 接收队列。
 * 不直接调用 ip_rcv()，避免递归。
 *
 * 参考：drivers/net/loopback.c loopback_xmit()
 */
static void loopback_xmit(struct sk_buff *skb)
{
    int idx = loopback_tail % LOOPBACK_QUEUE_SIZE;

    if ((loopback_tail - loopback_head) >= LOOPBACK_QUEUE_SIZE) {
        boot_printk("[net] WARN: loopback queue full, dropping packet\n");
        kfree_skb(skb);
        return;
    }

    loopback_queue[idx] = skb;
    loopback_tail++;
}

/*
 * net_rx_process - 处理 loopback 接收队列
 *
 * 扁平化处理所有排队的包。
 * 在 connect()、sendmsg() 等需要同步完成 loopback 通信的场景下调用。
 *
 * 参考：net/core/dev.c net_rx_action() (NAPI poll)
 */
void net_rx_process(void)
{
    while (loopback_head != loopback_tail) {
        int idx = loopback_head % LOOPBACK_QUEUE_SIZE;
        struct sk_buff *skb = loopback_queue[idx];
        loopback_head++;

        if (skb) {
            /*
             * 重置 skb 的 data 指针到 IP 头起始位置。
             * loopback 发送时 data 可能已经在 IP 头之后，
             * 需要重新从 IP 头开始处理。
             */
            if (skb->nh) {
                skb->data = (unsigned char *)skb->nh;
                skb->len = (unsigned int)(skb->tail - skb->data);
            }
            ip_rcv(skb);
        }
    }
}

/*
 * ip_queue_xmit - TCP 发包入口
 *
 * 参考：net/ipv4/ip_output.c ip_queue_xmit()
 *
 * 步骤：
 *   1. 在 skb 前面 push IP 头部空间
 *   2. 填充 IP 头部字段
 *   3. 执行 NF_INET_LOCAL_OUT 钩子
 *   4. 路由（本实现直接走 loopback）
 *   5. 执行 NF_INET_POST_ROUTING 钩子
 *   6. 调用 loopback_xmit() 发送
 */
void ip_queue_xmit(struct sock *sk, struct sk_buff *skb)
{
    struct iphdr *iph;
    unsigned int verdict;

    if (!sk || !skb)
        return;

    /* 在数据前 push IP 头部空间 */
    iph = (struct iphdr *)skb_push(skb, IP_HDR_LEN);
    skb->nh = iph;

    /* 填充 IP 头 */
    iph->version_ihl = 0x45;           /* IPv4, IHL=5 (20字节) */
    iph->tos         = 0;
    iph->tot_len     = htons((u16)skb->len);
    iph->id          = htons(ip_id_counter++);
    iph->frag_off    = 0;
    iph->ttl         = 64;             /* 默认 TTL */
    iph->protocol    = (u8)sk->sk_protocol;
    iph->check       = 0;              /* 简化：不计算校验和 */
    iph->saddr       = sk->sk_saddr;
    iph->daddr       = sk->sk_daddr;

    /* 设置协议 */
    skb->protocol = htons(ETH_P_IP);

    /* netfilter: NF_INET_LOCAL_OUT */
    verdict = nf_hook(&init_net, NF_INET_LOCAL_OUT, skb);
    if (verdict != NF_ACCEPT) {
        kfree_skb(skb);
        return;
    }

    /* netfilter: NF_INET_POST_ROUTING */
    verdict = nf_hook(&init_net, NF_INET_POST_ROUTING, skb);
    if (verdict != NF_ACCEPT) {
        kfree_skb(skb);
        return;
    }

    /* 发送到 loopback 设备 */
    loopback_xmit(skb);
}
