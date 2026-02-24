# Phase 11 Walkthrough: TCP/IP 协议栈 + netfilter 框架

> **目标**：实现 TCP/IP 网络栈和防火墙框架。
> **最终效果**：TCP 三次握手 + 数据收发 + netfilter 包过滤验证。

---

## 11.1 网络栈全景

```
  ┌─────────────────────────────────────────┐
  │  应用层                                  │
  │  socket API: socket/bind/listen/         │
  │  accept/connect/read/write               │
  ├─────────────────────────────────────────┤
  │  传输层 (TCP)                            │
  │  三次握手、可靠传输、序列号              │
  ├──────────┬──────────────────────────────┤
  │ netfilter│  网络层 (IP)                 │
  │  5 个钩子│  路由、分片、TTL              │
  ├──────────┴──────────────────────────────┤
  │  链路层 (VirtIO-net / loopback)         │
  └─────────────────────────────────────────┘
```

---

## 11.2 sk_buff — 网络数据包的载体

每个网络数据包用 `sk_buff` 结构表示：

```c
struct sk_buff {
    unsigned char *head;   /* 缓冲区起始 */
    unsigned char *data;   /* 当前数据起始 */
    unsigned char *tail;   /* 当前数据结束 */
    unsigned char *end;    /* 缓冲区结束 */

    unsigned int len;      /* 数据长度 (tail - data) */
    unsigned int protocol; /* 协议类型 */
    struct sock *sk;       /* 关联的 socket */
};
```

### 缓冲区操作

```
  alloc_skb() 后:
  ┌──────────────────────────────────────────────┐
  │                 2048 字节缓冲区               │
  head ──────────────────────────────────────── end
  data ──────────────────────────────────── tail
       (data = tail = head, 空的)

  skb_reserve(skb, 128) 后:
  ┌──────────────────────────────────────────────┐
  head ─── [128B 预留] ─── data/tail ──────── end
       TCP/IP 头部预留空间

  skb_put(skb, 100) 后 (添加应用数据):
  ┌──────────────────────────────────────────────┐
  head ─── [128B] ─── data ─── [100B 数据] ─── tail ── end

  skb_push(skb, 20) 后 (添加 TCP 头):
  ┌──────────────────────────────────────────────┐
  head ─── [108B] ─── data ─[TCP 20B]─[100B 数据]─ tail
                      ▲
                      data 前移

  skb_push(skb, 20) 后 (添加 IP 头):
  ┌──────────────────────────────────────────────┐
  head ─── [88B] ─ data─[IP 20B]─[TCP 20B]─[100B]─ tail
```

---

## 11.3 Socket 层

```c
struct sock {
    unsigned int state;          /* TCP_CLOSE, TCP_LISTEN, ... */
    unsigned int protocol;       /* IPPROTO_TCP */
    uint32_t saddr, daddr;       /* 源/目的 IP */
    uint16_t sport, dport;       /* 源/目的端口 */

    /* TCP 状态 */
    uint32_t snd_nxt;            /* 下一个发送序列号 */
    uint32_t rcv_nxt;            /* 期望接收的序列号 */
    uint32_t iss;                /* 初始发送序列号 */

    /* 接收缓冲区 */
    char rx_buf[2048];
    int rx_head, rx_tail;

    /* 监听队列 */
    struct sock *accept_queue[4];
    int accept_head, accept_tail;
};
```

### Socket API 实现

```c
int sys_socket(int domain, int type, int protocol)
{
    struct sock *sk = alloc_sock();
    sk->state = TCP_CLOSE;
    sk->protocol = IPPROTO_TCP;
    return sock_to_fd(sk);  /* 返回 fd */
}

int sys_bind(int fd, struct sockaddr_in *addr)
{
    struct sock *sk = fd_to_sock(fd);
    sk->saddr = addr->sin_addr;
    sk->sport = addr->sin_port;
    return 0;
}

int sys_listen(int fd, int backlog)
{
    struct sock *sk = fd_to_sock(fd);
    sk->state = TCP_LISTEN;
    return 0;
}
```

---

## 11.4 TCP 三次握手

### 握手流程

```
  Client                                Server
    │                                     │
    │  1. SYN (seq=ISS_C)                 │
    │ ─────────────────────────────────►   │
    │         TCP_SYN_SENT                 │ TCP_LISTEN
    │                                     │ → 创建子 socket
    │  2. SYN-ACK (seq=ISS_S, ack=ISS_C+1)│ TCP_SYN_RECV
    │ ◄─────────────────────────────────   │
    │ TCP_ESTABLISHED                      │
    │                                     │
    │  3. ACK (seq=ISS_C+1, ack=ISS_S+1)  │
    │ ─────────────────────────────────►   │
    │                                     │ TCP_ESTABLISHED
    │                                     │ → 加入 accept 队列
```

### loopback 队列 — 避免递归

同一线程既是 client 又是 server（loopback 测试），直接调用会导致无限递归。解决方案：**loopback 队列**。

```c
/* 环形缓冲区 */
static struct sk_buff *loopback_queue[64];
static int loopback_head, loopback_tail;

/* 发送：不直接处理，放入队列 */
void loopback_xmit(struct sk_buff *skb)
{
    loopback_queue[loopback_tail % 64] = skb;
    loopback_tail++;
}

/* 处理：手动排空队列 */
void net_rx_process(void)
{
    while (loopback_head != loopback_tail) {
        struct sk_buff *skb = loopback_queue[loopback_head % 64];
        loopback_head++;
        ip_rcv(skb);  /* 进入接收路径 */
    }
}
```

### Connect 实现（客户端）

```c
int tcp_v4_connect(struct sock *sk, uint32_t daddr, uint16_t dport)
{
    sk->daddr = daddr;
    sk->dport = dport;
    sk->iss = tcp_alloc_isn();      /* 初始序列号 */
    sk->snd_nxt = sk->iss + 1;

    /* 发送 SYN */
    tcp_send_syn(sk);               /* 构建 SYN 包并发送 */
    sk->state = TCP_SYN_SENT;

    /* 等待握手完成 */
    int timeout = 0;
    while (sk->state != TCP_ESTABLISHED && timeout < 1000) {
        net_rx_process();           /* 处理 loopback 队列 */
        timeout++;
    }

    return (sk->state == TCP_ESTABLISHED) ? 0 : -1;
}
```

### TCP 输入状态机

```c
void tcp_v4_rcv(struct sk_buff *skb)
{
    struct tcphdr *th = tcp_hdr(skb);
    struct sock *sk;

    /* 查找 socket: 先查已建立连接，再查监听 */
    sk = inet_lookup_established(th->dest, th->source);
    if (!sk)
        sk = inet_lookup_listener(th->dest);

    switch (sk->state) {
    case TCP_LISTEN:
        if (th->syn) {
            /* 收到 SYN → 创建子 socket → 发 SYN-ACK */
            struct sock *child = alloc_sock();
            child->state = TCP_SYN_RECV;
            child->rcv_nxt = ntohl(th->seq) + 1;
            child->iss = tcp_alloc_isn();
            tcp_send_synack(child);
        }
        break;

    case TCP_SYN_SENT:
        if (th->syn && th->ack) {
            /* 收到 SYN-ACK → 发 ACK → ESTABLISHED */
            sk->rcv_nxt = ntohl(th->seq) + 1;
            tcp_send_ack(sk);
            sk->state = TCP_ESTABLISHED;
        }
        break;

    case TCP_SYN_RECV:
        if (th->ack) {
            /* 收到 ACK → ESTABLISHED → 加入 accept 队列 */
            sk->state = TCP_ESTABLISHED;
            listener->accept_queue[listener->accept_tail++] = sk;
        }
        break;

    case TCP_ESTABLISHED:
        if (data_len > 0) {
            /* 收到数据 → 存入接收缓冲区 → 发 ACK */
            memcpy(sk->rx_buf + sk->rx_tail, data, data_len);
            sk->rx_tail += data_len;
            sk->rcv_nxt += data_len;
            tcp_send_ack(sk);
        }
        break;
    }
}
```

---

## 11.5 IP 层

### 发送路径

```c
int ip_queue_xmit(struct sk_buff *skb)
{
    /* 添加 IP 头 */
    struct iphdr *iph = skb_push(skb, sizeof(struct iphdr));
    iph->version = 4;
    iph->ihl = 5;                    /* 20 字节 */
    iph->tot_len = htons(skb->len);
    iph->ttl = 64;
    iph->protocol = IPPROTO_TCP;
    iph->saddr = htonl(sk->saddr);
    iph->daddr = htonl(sk->daddr);
    iph->check = ip_checksum(iph);

    /* 经过 netfilter 钩子 */
    if (nf_hook(NF_INET_LOCAL_OUT, skb) == NF_DROP)
        return -1;

    /* 发送到 loopback */
    loopback_xmit(skb);
    return 0;
}
```

### 接收路径

```c
int ip_rcv(struct sk_buff *skb)
{
    struct iphdr *iph = (struct iphdr *)skb->data;

    /* netfilter PRE_ROUTING */
    if (nf_hook(NF_INET_PRE_ROUTING, skb) == NF_DROP)
        return -1;

    /* netfilter LOCAL_IN */
    if (nf_hook(NF_INET_LOCAL_IN, skb) == NF_DROP)
        return -1;

    /* 去掉 IP 头，交给传输层 */
    skb_pull(skb, iph->ihl * 4);

    if (iph->protocol == IPPROTO_TCP)
        tcp_v4_rcv(skb);

    return 0;
}
```

---

## 11.6 netfilter — 内核防火墙框架

### 5 个钩子点

```
                 ┌──────────────┐
  网络 ─────────►│ PRE_ROUTING  │
  接收           └──────┬───────┘
                        │
                 ┌──────┴───────┐
                 │   路由决策    │
                 └──┬───────┬───┘
              本机  │       │ 转发
           ┌───────┴──┐ ┌──┴────────┐
           │ LOCAL_IN  │ │  FORWARD   │
           └─────┬────┘ └────┬──────┘
                 │           │
           ┌─────┴────┐     │
           │  应用层   │     │
           └─────┬────┘     │
                 │           │
           ┌─────┴────┐     │
           │ LOCAL_OUT │     │
           └─────┬────┘     │
                 │      ┌───┴────────┐
                 └──────┤POST_ROUTING │───────► 网络发送
                        └────────────┘
```

### 注册钩子

```c
struct nf_hook_ops {
    nf_hookfn *hook;           /* 回调函数 */
    int hooknum;               /* 钩子点 */
    int priority;              /* 优先级（小 = 先执行） */
};

void nf_register_net_hook(struct nf_hook_ops *ops)
{
    /* 按优先级插入到对应钩子点的链表 */
    struct nf_hook_entries *entries = &nf_hooks[ops->hooknum];
    /* ... 排序插入 ... */
}
```

### 执行钩子链

```c
int nf_hook_slow(int hooknum, struct sk_buff *skb)
{
    struct nf_hook_entries *entries = &nf_hooks[hooknum];

    for (int i = 0; i < entries->num_hooks; i++) {
        int verdict = entries->hooks[i].hook(skb);

        switch (verdict) {
        case NF_ACCEPT:
            continue;        /* 继续下一个钩子 */
        case NF_DROP:
            kfree_skb(skb);
            return NF_DROP;  /* 丢包 */
        case NF_STOLEN:
            return NF_STOLEN; /* 钩子接管了包 */
        }
    }
    return NF_ACCEPT;
}
```

### nftables 规则

```c
struct nft_rule {
    uint32_t src_ip, src_mask;    /* 源 IP 匹配 */
    uint32_t dst_ip, dst_mask;    /* 目的 IP 匹配 */
    uint8_t protocol;             /* 协议匹配 */
    uint16_t sport, dport;        /* 端口匹配 */
    int target;                   /* NF_ACCEPT 或 NF_DROP */
    int valid;
};

int nft_do_chain(struct nft_chain *chain, struct sk_buff *skb)
{
    struct iphdr *iph = ip_hdr(skb);

    for (int i = 0; i < chain->num_rules; i++) {
        struct nft_rule *rule = &chain->rules[i];

        /* 匹配所有条件 */
        if ((iph->saddr & rule->src_mask) == rule->src_ip &&
            (iph->daddr & rule->dst_mask) == rule->dst_ip &&
            iph->protocol == rule->protocol) {
            return rule->target;  /* 第一个匹配的规则决定 */
        }
    }
    return NF_ACCEPT;  /* 默认放行 */
}
```

---

## 11.7 测试验证

```c
static void test_phase11(void)
{
    /* TCP loopback 测试 */
    int server_fd = sys_socket(AF_INET, SOCK_STREAM, 0);
    sys_bind(server_fd, &server_addr);  /* port=8080 */
    sys_listen(server_fd, 4);

    int client_fd = sys_socket(AF_INET, SOCK_STREAM, 0);
    sys_connect(client_fd, &server_addr);
    /* 三次握手通过 loopback 队列完成 */

    int conn_fd = sys_accept(server_fd);
    /* conn_fd = 已建立连接的 socket */

    /* 数据收发 */
    sock_write(client_fd, "hello tcp\n", 10);
    net_rx_process();
    char buf[32];
    sock_read(conn_fd, buf, 32);
    /* buf == "hello tcp\n" */

    /* netfilter 测试 */
    struct nf_hook_ops drop_hook = {
        .hook = test_drop_fn,        /* 返回 NF_DROP */
        .hooknum = NF_INET_LOCAL_IN,
        .priority = 0
    };
    nf_register_net_hook(&drop_hook);
    /* 后续入站包被丢弃 */
}
```

---

## 11.8 Phase 11 核心概念总结

| 概念 | 说明 |
|------|------|
| **sk_buff** | 网络包缓冲区，push/pull 操作头部 |
| **TCP 三次握手** | SYN → SYN-ACK → ACK |
| **loopback 队列** | 避免同线程收发递归 |
| **IP 层** | 添加/解析 IP 头，分发到传输层 |
| **netfilter** | 5 个钩子点的包过滤框架 |
| **nftables** | 规则匹配引擎 (IP/端口/协议) |
| **NF_ACCEPT/DROP** | 钩子返回值：放行/丢弃 |

**Phase 11 奠定的基础**：有了网络栈，容器可以通信。Phase 12 将添加安全控制。
