/* SPDX-License-Identifier: GPL-2.0 */
/*
 * arm64os/net/ipv4/tcp_output.c
 *
 * TCP 发包 — 控制报文（SYN、SYN-ACK、ACK）+ connect()
 *
 * 参考：net/ipv4/tcp_output.c
 *       net/ipv4/tcp_ipv4.c
 *
 * Phase 11 实现：
 *   - tcp_v4_connect()：客户端发起连接（SYN + 三次握手驱动）
 *   - tcp_send_synack()：服务端发送 SYN-ACK
 *   - tcp_send_ack()：发送 ACK
 *   - tcp_transmit_skb()：公共发送辅助
 *
 * 简化说明：
 *   - 无 TCP 选项（MSS、Timestamps 等）
 *   - 无重传定时器
 *   - 无拥塞窗口
 *   - 固定窗口大小 8192
 *   - 通过 loopback 队列同步完成握手
 */

#include <linux/types.h>
#include <linux/net.h>
#include <linux/skbuff.h>

/* 外部函数声明 */
void boot_printk(const char *s);
void boot_printk_hex(unsigned long val);

/* IP 层发送（net/ipv4/ip_output.c 提供）*/
void ip_queue_xmit(struct sock *sk, struct sk_buff *skb);

/* loopback 队列处理（net/ipv4/ip_output.c 提供）*/
void net_rx_process(void);

/* ISN 分配（net/ipv4/tcp.c 提供）*/
u32 tcp_alloc_isn(void);

/*
 * tcp_transmit_skb - 构造 TCP 头并通过 IP 层发送
 *
 * 参考：net/ipv4/tcp_output.c tcp_transmit_skb()
 *
 * @sk:    关联的 socket
 * @skb:   已预留 headroom 的 skb（data 指向 payload 或 reserve 之后）
 * @flags: TCP 标志位（TCPHDR_SYN、TCPHDR_ACK 等组合）
 * @seq:   序列号
 * @ack:   确认号
 */
static void tcp_transmit_skb(struct sock *sk, struct sk_buff *skb,
                              u8 flags, u32 seq, u32 ack)
{
    struct tcphdr *th;

    /* 在 payload 前面 push TCP 头 */
    th = (struct tcphdr *)skb_push(skb, TCP_HDR_LEN);
    skb->th = th;

    /* 填充 TCP 头 */
    th->source   = sk->sk_sport;
    th->dest     = sk->sk_dport;
    th->seq      = htonl(seq);
    th->ack_seq  = htonl(ack);
    th->doff_res = (TCP_HDR_LEN / 4) << 4;     /* 数据偏移 = 5 */
    th->flags    = flags;
    th->window   = htons(8192);                  /* 固定窗口 */
    th->check    = 0;                            /* 简化：不计算校验和 */
    th->urg_ptr  = 0;

    /* 通过 IP 层发送 */
    ip_queue_xmit(sk, skb);
}

/*
 * tcp_send_synack - 发送 SYN-ACK（三次握手第二步）
 *
 * 参考：net/ipv4/tcp_output.c tcp_send_synack()
 *
 * 由服务端子连接在收到 SYN 后调用。
 * 发送 SYN + ACK，序列号为 ISS，确认号为对端 ISS + 1。
 */
void tcp_send_synack(struct sock *sk)
{
    struct sk_buff *skb;

    skb = alloc_skb(IP_HDR_LEN + TCP_HDR_LEN);
    if (!skb)
        return;

    skb->sk = sk;

    /* 预留 IP + TCP 头空间（TCP 头由 tcp_transmit_skb push，IP 头由 ip_queue_xmit push）*/
    skb_reserve(skb, IP_HDR_LEN + TCP_HDR_LEN);

    /* 发送 SYN + ACK */
    tcp_transmit_skb(sk, skb, TCPHDR_SYN | TCPHDR_ACK,
                     sk->iss, sk->rcv_nxt);

    /* SYN 消耗 1 个序列号 */
    sk->snd_nxt = sk->iss + 1;
}

/*
 * tcp_send_ack - 发送纯 ACK
 *
 * 参考：net/ipv4/tcp_output.c tcp_send_ack()
 *
 * 用于：
 *   - 三次握手第三步（客户端 → 服务端）
 *   - 数据确认
 */
void tcp_send_ack(struct sock *sk)
{
    struct sk_buff *skb;

    skb = alloc_skb(IP_HDR_LEN + TCP_HDR_LEN);
    if (!skb)
        return;

    skb->sk = sk;

    /* 预留 IP + TCP 头空间 */
    skb_reserve(skb, IP_HDR_LEN + TCP_HDR_LEN);

    /* 发送纯 ACK */
    tcp_transmit_skb(sk, skb, TCPHDR_ACK,
                     sk->snd_nxt, sk->rcv_nxt);
}

/*
 * tcp_v4_connect - 客户端发起 TCP 连接（三次握手）
 *
 * 参考：net/ipv4/tcp_ipv4.c tcp_v4_connect()
 *       net/ipv4/tcp_output.c tcp_connect()
 *
 * 流程：
 *   1. 设置对端地址
 *   2. 分配源端口（如未绑定）
 *   3. 设置本地地址为 loopback
 *   4. 分配 ISN
 *   5. 发送 SYN（三次握手第一步）
 *   6. 进入 SYN_SENT 状态
 *   7. 驱动 loopback 队列处理，完成三次握手
 *
 * 返回：0 成功，-1 失败
 */
int tcp_v4_connect(struct sock *sk, __be32 daddr, __be16 dport)
{
    struct sk_buff *skb;
    int max_rounds = 10;
    static __be16 ephemeral_port = 0;

    if (!sk)
        return -1;

    /* 设置对端地址 */
    sk->sk_daddr = daddr;
    sk->sk_dport = dport;

    /* 设置本地地址为 loopback（如未绑定）*/
    if (!sk->sk_bound) {
        sk->sk_saddr = htonl(INADDR_LOOPBACK);
        /* 分配临时端口（从 49152 开始）*/
        if (ephemeral_port == 0)
            ephemeral_port = htons(49152);
        sk->sk_sport = ephemeral_port;
        ephemeral_port = htons(ntohs(ephemeral_port) + 1);
        sk->sk_bound = 1;
    }

    /* 分配初始序列号 */
    sk->iss = tcp_alloc_isn();
    sk->snd_nxt = sk->iss;
    sk->snd_una = sk->iss;

    /* 构造并发送 SYN（三次握手第一步）*/
    skb = alloc_skb(IP_HDR_LEN + TCP_HDR_LEN);
    if (!skb)
        return -1;

    skb->sk = sk;
    skb_reserve(skb, IP_HDR_LEN + TCP_HDR_LEN);

    /* SYN 消耗 1 个序列号 */
    tcp_transmit_skb(sk, skb, TCPHDR_SYN, sk->iss, 0);
    sk->snd_nxt = sk->iss + 1;

    /* 进入 SYN_SENT 状态 */
    sk->sk_state = TCP_SYN_SENT;

    /*
     * 驱动 loopback 队列处理，完成整个三次握手：
     *   1. SYN → loopback → 服务端处理 → 发送 SYN-ACK → loopback
     *   2. SYN-ACK → 客户端处理 → 发送 ACK → loopback
     *   3. ACK → 服务端子连接处理 → ESTABLISHED + 加入 accept 队列
     *
     * 每次 net_rx_process() 处理一批排队的包。
     * 循环直到连接建立或超过最大轮数。
     */
    while (sk->sk_state != TCP_ESTABLISHED && max_rounds-- > 0) {
        net_rx_process();
    }

    if (sk->sk_state == TCP_ESTABLISHED)
        return 0;

    /* 握手失败 */
    sk->sk_state = TCP_CLOSE;
    return -1;
}
