/* SPDX-License-Identifier: GPL-2.0 */
/*
 * arm64os/net/ipv4/tcp_input.c
 *
 * TCP 收包处理 + 状态机
 *
 * 参考：net/ipv4/tcp_input.c
 *       net/ipv4/tcp_ipv4.c
 *
 * Phase 11 实现：
 *   - tcp_v4_rcv()：TCP 收包入口（从 ip_local_deliver 调用）
 *   - TCP 状态机：处理 SYN、SYN-ACK、ACK、数据
 *   - 三次握手：
 *     客户端：CLOSE → SYN_SENT → ESTABLISHED
 *     服务端：LISTEN → (创建子连接 SYN_RECV) → ESTABLISHED
 *   - 数据接收：将 payload 放入 sock 的 rx_buf
 *
 * 简化说明：
 *   - 无超时重传（loopback 不丢包）
 *   - 无拥塞控制
 *   - 无 RST 处理
 *   - 无 FIN 四次挥手
 *   - 无 TIME_WAIT
 */

#include <linux/types.h>
#include <linux/net.h>
#include <linux/skbuff.h>

/* 外部函数声明 */
void boot_printk(const char *s);
void boot_printk_hex(unsigned long val);

/* TCP 发送函数（net/ipv4/tcp_output.c 提供）*/
void tcp_send_synack(struct sock *sk);
void tcp_send_ack(struct sock *sk);

/* ISN 分配（net/ipv4/tcp.c 提供）*/
u32 tcp_alloc_isn(void);

/* socket 池操作（net/core/sock.c 提供）*/
struct sock *sk_alloc(void);
struct sock *inet_lookup_listener(__be32 daddr, __be16 dport);
struct sock *inet_lookup_established(__be32 saddr, __be16 sport,
                                     __be32 daddr, __be16 dport);

/*
 * tcp_rcv_synsent_state_process - SYN_SENT 状态收到 SYN-ACK
 *
 * 参考：net/ipv4/tcp_input.c tcp_rcv_synsent_state_process()
 *
 * 客户端在 connect() 发出 SYN 后处于 SYN_SENT 状态。
 * 收到 SYN-ACK → 更新序列号 → 发送 ACK → 进入 ESTABLISHED。
 */
static void tcp_rcv_synsent_state_process(struct sock *sk, struct sk_buff *skb)
{
    struct tcphdr *th = skb->th;

    if (!th)
        return;

    /* 期望收到 SYN + ACK */
    if (!(th->flags & TCPHDR_SYN) || !(th->flags & TCPHDR_ACK))
        return;

    /* 更新序列号 */
    sk->irs = ntohl(th->seq);
    sk->rcv_nxt = ntohl(th->seq) + 1;   /* SYN 消耗 1 个序列号 */
    sk->snd_una = ntohl(th->ack_seq);

    /* 发送 ACK（三次握手第三步）*/
    tcp_send_ack(sk);

    /* 进入 ESTABLISHED 状态 */
    sk->sk_state = TCP_ESTABLISHED;
}

/*
 * tcp_rcv_listen_state_process - LISTEN 状态收到 SYN
 *
 * 服务端监听 socket 收到 SYN：
 *   1. 创建子 sock（继承本地地址/端口）
 *   2. 设置对端地址信息
 *   3. 分配 ISN
 *   4. 发送 SYN-ACK
 *   5. 子连接进入 SYN_RECV 状态
 */
static void tcp_rcv_listen_state_process(struct sock *listener,
                                          struct sk_buff *skb)
{
    struct tcphdr *th = skb->th;
    struct iphdr *iph = skb->nh;
    struct sock *child;

    if (!th || !iph)
        return;

    /* 仅处理 SYN（不含 ACK） */
    if (!(th->flags & TCPHDR_SYN) || (th->flags & TCPHDR_ACK))
        return;

    /* 创建子连接 sock */
    child = sk_alloc();
    if (!child) {
        boot_printk("[tcp] WARN: cannot allocate child sock\n");
        return;
    }

    /* 继承监听 socket 的本地地址 */
    child->sk_saddr = listener->sk_saddr;
    child->sk_sport = listener->sk_sport;

    /* 设置对端地址（从 IP/TCP 头中提取）*/
    child->sk_daddr = iph->saddr;
    child->sk_dport = th->source;

    /* 如果监听方绑定 INADDR_ANY，使用包的目的地址作为本地地址 */
    if (child->sk_saddr == 0)
        child->sk_saddr = iph->daddr;

    /* TCP 序列号 */
    child->iss = tcp_alloc_isn();
    child->snd_nxt = child->iss;
    child->snd_una = child->iss;
    child->irs = ntohl(th->seq);
    child->rcv_nxt = ntohl(th->seq) + 1;   /* SYN 消耗 1 个序列号 */

    /* 关联到监听 socket */
    child->sk_listener = listener;

    /* 进入 SYN_RECV 状态 */
    child->sk_state = TCP_SYN_RECV;

    /* 发送 SYN-ACK（三次握手第二步）*/
    tcp_send_synack(child);
}

/*
 * tcp_rcv_synrecv_state_process - SYN_RECV 状态收到 ACK
 *
 * 服务端子连接在发出 SYN-ACK 后处于 SYN_RECV 状态。
 * 收到客户端的 ACK → 三次握手完成 → ESTABLISHED。
 * 将子连接加入监听 socket 的 accept 队列。
 */
static void tcp_rcv_synrecv_state_process(struct sock *sk, struct sk_buff *skb)
{
    struct tcphdr *th = skb->th;
    struct sock *listener;
    int idx;

    if (!th)
        return;

    /* 期望收到 ACK（不含 SYN） */
    if (!(th->flags & TCPHDR_ACK) || (th->flags & TCPHDR_SYN))
        return;

    /* 更新发送确认号 */
    sk->snd_una = ntohl(th->ack_seq);

    /* 进入 ESTABLISHED 状态 */
    sk->sk_state = TCP_ESTABLISHED;

    /* 加入监听 socket 的 accept 队列 */
    listener = sk->sk_listener;
    if (listener) {
        idx = listener->accept_tail % SOCK_ACCEPT_MAX;
        listener->accept_queue[idx] = sk;
        listener->accept_tail++;
    }
}

/*
 * tcp_rcv_established_data - ESTABLISHED 状态收到数据
 *
 * 将 TCP payload 放入 sock 的接收缓冲区。
 */
static void tcp_rcv_established_data(struct sock *sk, struct sk_buff *skb)
{
    struct tcphdr *th = skb->th;
    unsigned char *payload;
    unsigned int payload_len;
    unsigned int tcp_hdr_len;
    unsigned int i;

    if (!th)
        return;

    /* 计算 TCP 头长度（doff 字段，单位 4 字节）*/
    tcp_hdr_len = (unsigned int)((th->doff_res >> 4) & 0x0f) * 4;
    if (tcp_hdr_len < TCP_HDR_LEN)
        tcp_hdr_len = TCP_HDR_LEN;

    /* payload 起始 = TCP 头之后 */
    payload = (unsigned char *)th + tcp_hdr_len;

    /*
     * payload_len = skb 的总 IP payload - IP头 - TCP头
     * 此时 skb->data 指向 TCP 头（IP 头已被 ip_local_deliver 剥离），
     * skb->len = TCP头 + payload
     */
    if (skb->len <= tcp_hdr_len)
        return;  /* 无 payload（纯 ACK） */

    payload_len = skb->len - tcp_hdr_len;

    /* 将 payload 写入 sock 的接收缓冲区 */
    for (i = 0; i < payload_len; i++) {
        unsigned int idx = (unsigned int)sk->rx_tail % SOCK_RX_BUF_SIZE;
        sk->rx_buf[idx] = payload[i];
        sk->rx_tail++;
    }

    /* 更新接收序列号 */
    sk->rcv_nxt += payload_len;

    /* 发送 ACK 确认收到数据 */
    tcp_send_ack(sk);
}

/*
 * tcp_v4_rcv - TCP 收包入口
 *
 * 参考：net/ipv4/tcp_ipv4.c tcp_v4_rcv()
 *
 * 由 ip_local_deliver() 在剥离 IP 头后调用。
 * 此时 skb->data 指向 TCP 头，skb->nh 指向 IP 头。
 *
 * 处理步骤：
 *   1. 解析 TCP 头
 *   2. 查找匹配的 socket（先找已建立连接，再找监听 socket）
 *   3. 根据 socket 状态进入对应的状态机处理
 */
void tcp_v4_rcv(struct sk_buff *skb)
{
    struct tcphdr *th;
    struct iphdr *iph;
    struct sock *sk;

    if (!skb || skb->len < TCP_HDR_LEN) {
        kfree_skb(skb);
        return;
    }

    /* 解析 TCP 头 */
    th = (struct tcphdr *)skb->data;
    skb->th = th;

    /* IP 头（在 ip_rcv 中已设置）*/
    iph = skb->nh;
    if (!iph) {
        kfree_skb(skb);
        return;
    }

    /*
     * 查找匹配的 socket：
     * 1. 先按四元组查找已建立连接（ESTABLISHED / SYN_RECV / SYN_SENT）
     * 2. 若未找到，按目的端口查找监听 socket
     */
    sk = inet_lookup_established(iph->saddr, th->source,
                                  iph->daddr, th->dest);

    if (!sk) {
        /* 尝试查找监听 socket */
        sk = inet_lookup_listener(iph->daddr, th->dest);
    }

    if (!sk) {
        /* 无匹配 socket，丢弃（应发送 RST，简化省略）*/
        kfree_skb(skb);
        return;
    }

    /* 根据 socket 状态处理 */
    switch (sk->sk_state) {
    case TCP_LISTEN:
        /* 服务端收到 SYN → 创建子连接 + 发送 SYN-ACK */
        tcp_rcv_listen_state_process(sk, skb);
        break;

    case TCP_SYN_SENT:
        /* 客户端收到 SYN-ACK → 发送 ACK → ESTABLISHED */
        tcp_rcv_synsent_state_process(sk, skb);
        break;

    case TCP_SYN_RECV:
        /* 服务端子连接收到 ACK → ESTABLISHED + 加入 accept 队列 */
        tcp_rcv_synrecv_state_process(sk, skb);
        break;

    case TCP_ESTABLISHED:
        /* 已建立连接收到数据 */
        tcp_rcv_established_data(sk, skb);
        break;

    default:
        break;
    }

    /* 释放 skb */
    kfree_skb(skb);
}
