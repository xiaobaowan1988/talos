/* SPDX-License-Identifier: GPL-2.0 */
/*
 * arm64os/net/ipv4/tcp.c
 *
 * TCP 协议主体 — 数据发送/接收 + 初始化
 *
 * 参考：net/ipv4/tcp.c
 *
 * Phase 11 实现：
 *   - tcp_init()：TCP 子系统初始化
 *   - tcp_sendmsg()：发送数据到已建立连接
 *   - tcp_recvmsg()：从接收缓冲区读取数据
 *
 * 简化说明：
 *   - 无 Nagle 算法
 *   - 无拥塞控制
 *   - 无滑动窗口（使用固定窗口大小）
 *   - 无 OOB / URG 数据
 *   - 通过 loopback 同步收发
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

/* TCP 序列号全局计数器（简化版 ISN 生成）*/
static u32 tcp_seq_counter = 1000;

/*
 * tcp_alloc_isn - 分配初始序列号
 *
 * 参考：net/ipv4/tcp_ipv4.c secure_tcp_seq()
 *
 * 简化版：每个连接递增 1000，避免重叠。
 */
u32 tcp_alloc_isn(void)
{
    u32 isn = tcp_seq_counter;
    tcp_seq_counter += 1000;
    return isn;
}

/*
 * tcp_init - TCP 子系统初始化
 *
 * 参考：net/ipv4/tcp.c tcp_init()
 */
void tcp_init(void)
{
    tcp_seq_counter = 1000;
    boot_printk("[net] TCP subsystem initialized\n");
}

/*
 * tcp_sendmsg - 在已建立连接上发送数据
 *
 * 参考：net/ipv4/tcp.c tcp_sendmsg()
 *
 * 流程：
 *   1. 分配 skb
 *   2. 预留 IP + TCP 头空间
 *   3. 复制用户数据到 skb
 *   4. 构造 TCP 头（PSH + ACK）
 *   5. 调用 ip_queue_xmit() 发送
 *   6. 处理 loopback 队列（同步投递到接收方）
 *
 * 返回：发送的字节数，或 -1 失败
 */
int tcp_sendmsg(struct sock *sk, const void *buf, size_t len)
{
    struct sk_buff *skb;
    struct tcphdr *th;
    unsigned char *payload;
    unsigned int i;

    if (!sk || !buf || len == 0)
        return -1;
    if (sk->sk_state != TCP_ESTABLISHED)
        return -1;
    if (len > SKB_DATA_SIZE - IP_HDR_LEN - TCP_HDR_LEN - 64)
        return -1;  /* 数据太大 */

    /* 分配 skb */
    skb = alloc_skb((unsigned int)(IP_HDR_LEN + TCP_HDR_LEN + len));
    if (!skb)
        return -1;

    skb->sk = sk;

    /* 预留 IP + TCP 头空间 */
    skb_reserve(skb, IP_HDR_LEN + TCP_HDR_LEN);

    /* 复制应用数据 */
    payload = skb_put(skb, (unsigned int)len);
    for (i = 0; i < len; i++)
        payload[i] = ((const unsigned char *)buf)[i];

    /* 构造 TCP 头 */
    th = (struct tcphdr *)skb_push(skb, TCP_HDR_LEN);
    skb->th = th;

    th->source   = sk->sk_sport;
    th->dest     = sk->sk_dport;
    th->seq      = htonl(sk->snd_nxt);
    th->ack_seq  = htonl(sk->rcv_nxt);
    th->doff_res = (TCP_HDR_LEN / 4) << 4;     /* 数据偏移 = 5 (20字节) */
    th->flags    = TCPHDR_PSH | TCPHDR_ACK;     /* PSH + ACK */
    th->window   = htons(8192);                  /* 固定窗口大小 */
    th->check    = 0;                            /* 简化：不计算校验和 */
    th->urg_ptr  = 0;

    /* 更新发送序列号 */
    sk->snd_nxt += (u32)len;

    /* 通过 IP 层发送 */
    ip_queue_xmit(sk, skb);

    /* 处理 loopback 队列，同步投递到接收方 */
    net_rx_process();

    return (int)len;
}

/*
 * tcp_recvmsg - 从接收缓冲区读取数据
 *
 * 参考：net/ipv4/tcp.c tcp_recvmsg()
 *
 * 从 sock 的 rx_buf 环形缓冲区中读取数据。
 * 数据由 tcp_v4_rcv()（tcp_input.c）在收到数据包时写入。
 *
 * 返回：读取的字节数，或 -1 失败
 */
int tcp_recvmsg(struct sock *sk, void *buf, size_t len)
{
    unsigned int available;
    unsigned int copy_len;
    unsigned int i;

    if (!sk || !buf || len == 0)
        return -1;

    /* 计算可用数据量 */
    available = (unsigned int)(sk->rx_tail - sk->rx_head);
    if (available == 0)
        return 0;  /* 无数据 */

    copy_len = (unsigned int)len;
    if (copy_len > available)
        copy_len = available;

    /* 从环形缓冲区复制数据 */
    for (i = 0; i < copy_len; i++) {
        ((unsigned char *)buf)[i] =
            sk->rx_buf[(sk->rx_head + i) % SOCK_RX_BUF_SIZE];
    }

    sk->rx_head += copy_len;

    return (int)copy_len;
}
