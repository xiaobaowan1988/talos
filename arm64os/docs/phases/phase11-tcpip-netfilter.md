# Phase 11：TCP/IP协议栈 + netfilter框架

## 参考内核文件

```
include/linux/skbuff.h      # sk_buff数据结构
include/linux/netfilter.h   # netfilter钩子定义
include/linux/net.h         # 网络核心数据结构（sock, iphdr, tcphdr）
include/linux/types.h       # 基础类型（新增 __be16/__be32/__wsum）
net/core/sock.c             # socket核心（sk_buff、socket创建）
net/core/skbuff.c           # sk_buff（网络数据包缓冲区）
net/ipv4/af_inet.c          # IPv4地址族（socket系统调用接入）
net/ipv4/ip_input.c         # IP收包处理（ip_rcv）
net/ipv4/ip_output.c        # IP发包处理（ip_queue_xmit）+ loopback队列
net/ipv4/tcp.c              # TCP协议主体（sendmsg/recvmsg）
net/ipv4/tcp_input.c        # TCP收包（三次握手状态机、数据接收）
net/ipv4/tcp_output.c       # TCP发包（SYN/SYN-ACK/ACK + connect）
net/netfilter/core.c        # netfilter钩子框架
net/netfilter/nf_tables_core.c  # nftables规则引擎
```

---

## 11.1 网络数据包生命周期

```
接收方向（RX）：
NIC 硬件中断
    │ DMA → skb
    ▼
net_rx_action()             ← 软中断（NAPI poll）
    │
    ▼
netif_receive_skb()
    │
    ├─→ [netfilter: NF_INET_PRE_ROUTING]
    │
    ▼
ip_rcv()                    ← IP层接收
    │ 路由查找
    ├─→ 本机目的: ip_local_deliver()
    │       │
    │       ├─→ [netfilter: NF_INET_LOCAL_IN]
    │       │
    │       ▼
    │   tcp_v4_rcv()         ← TCP层接收
    │       │
    │       ▼
    │   socket接收队列 → 用户态 recv()
    │
    └─→ 转发: ip_forward()
            │
            ├─→ [netfilter: NF_INET_FORWARD]
            └─→ ip_output()

发送方向（TX）：
用户态 send()
    │
    ▼
tcp_sendmsg()
    │
    ▼
ip_queue_xmit()
    │
    ├─→ [netfilter: NF_INET_LOCAL_OUT]
    │
    ▼
dev_queue_xmit()
    │
    ▼
virtio_net 驱动 → NIC
```

## 11.2 sk_buff（网络包缓冲区）

```c
/* 参考 include/linux/skbuff.h */
/* sk_buff 是内核网络栈中最核心的数据结构 */

/* 实际实现：简化版，内联数据缓冲区，无分散/聚集 I/O，静态池分配 */
struct sk_buff {
    /* 包数据指针 */
    unsigned char   *head;   /* 缓冲区起始（固定）*/
    unsigned char   *data;   /* 当前有效数据起始（随协议层移动）*/
    unsigned char   *tail;   /* 当前有效数据结束 */
    unsigned char   *end;    /* 缓冲区结束（固定）*/

    /* 元数据 */
    struct sock     *sk;        /* 关联的socket */
    __be16          protocol;   /* ETH_P_IP, ETH_P_ARP 等 */
    unsigned int    len;        /* 数据总长度（data 到 tail）*/

    /* 协议头指针（各层处理时设置）*/
    struct iphdr    *nh;        /* 网络层头部（IP）*/
    struct tcphdr   *th;        /* 传输层头部（TCP）*/

    /* 池管理 */
    int             used;       /* 是否已分配 */

    /* 内联数据缓冲区（SKB_DATA_SIZE=2048）*/
    unsigned char   buf[SKB_DATA_SIZE];
};

/* skb 操作：各层添加/去除头部 */
/* 添加头部：data 向前移动 */
static inline void *skb_push(struct sk_buff *skb, unsigned int len) {
    skb->data -= len;
    skb->len  += len;
    return skb->data;
}

/* 去除头部：data 向后移动 */
static inline void *skb_pull(struct sk_buff *skb, unsigned int len) {
    skb->len  -= len;
    return skb->data += len;
}
```

## 11.3 Socket系统调用层（参考 net/socket.c）

```c
/* 实际实现：简化版，静态池分配，socket 描述符 = 池索引 */

/* sys_socket(AF_INET, SOCK_STREAM, 0) */
int sys_socket(int family, int type, int protocol) {
    struct sock *sk = sk_alloc();   /* 从静态池分配 */
    sk->sk_family = family;
    sk->sk_type = type;
    sk->sk_protocol = (protocol == 0) ? IPPROTO_TCP : protocol;
    sk->sk_state = TCP_CLOSE;
    return sock_fd(sk);             /* 返回池索引作为 fd */
}

/* sys_bind(sockfd, &addr, sizeof(addr)) */
int sys_bind(int sockfd, const struct sockaddr_in *addr, int addrlen);

/* sys_listen(sockfd, backlog) */
int sys_listen(int sockfd, int backlog);

/* sys_accept(sockfd, addr, addrlen) */
int sys_accept(int sockfd, struct sockaddr_in *addr, int *addrlen);

/* sys_connect(sockfd, &addr, sizeof(addr)) */
int sys_connect(int sockfd, const struct sockaddr_in *addr, int addrlen);

/* 注意：socket I/O 使用 sock_write/sock_read（非 sys_write/sys_read）*/
ssize_t sock_write(int sockfd, const void *buf, size_t len);
ssize_t sock_read(int sockfd, void *buf, size_t len);
```

## 11.4 TCP三次握手（参考 tcp_input.c + tcp_output.c）

```c
/*
 * 实际实现：通过 loopback 队列同步完成三次握手
 *
 * loopback 架构避免递归（ip_queue_xmit → ip_rcv → tcp_v4_rcv → ip_queue_xmit）：
 *   - ip_queue_xmit → loopback_xmit(): 入队列（不直接调用 ip_rcv）
 *   - net_rx_process(): 出队列，平坦循环处理所有排队包
 *   - tcp_v4_connect() 循环调用 net_rx_process() 直到 ESTABLISHED
 */

/* 客户端 connect() */
int tcp_v4_connect(struct sock *sk, __be32 daddr, __be16 dport) {
    /* 1. 设置对端地址 */
    sk->sk_daddr = daddr;
    sk->sk_dport = dport;

    /* 2. 如未绑定，分配 loopback 地址 + 临时端口 */
    if (!sk->sk_bound) {
        sk->sk_saddr = htonl(INADDR_LOOPBACK);
        sk->sk_sport = htons(ephemeral_port++);
    }

    /* 3. 分配 ISN，发送 SYN */
    sk->iss = tcp_alloc_isn();
    tcp_transmit_skb(sk, skb, TCPHDR_SYN, sk->iss, 0);
    sk->sk_state = TCP_SYN_SENT;

    /* 4. 驱动 loopback 队列完成握手 */
    while (sk->sk_state != TCP_ESTABLISHED && max_rounds-- > 0)
        net_rx_process();

    return (sk->sk_state == TCP_ESTABLISHED) ? 0 : -1;
}

/* 服务端收到 SYN → 创建子连接 + 发送 SYN-ACK */
void tcp_rcv_listen_state_process(struct sock *listener, struct sk_buff *skb) {
    struct sock *child = sk_alloc();
    /* 设置子连接地址、发送 SYN-ACK */
    tcp_send_synack(child);
}

/* 客户端收到 SYN-ACK → 发送 ACK → ESTABLISHED */
void tcp_rcv_synsent_state_process(struct sock *sk, struct sk_buff *skb) {
    sk->rcv_nxt = ntohl(th->seq) + 1;
    sk->snd_una = ntohl(th->ack_seq);
    tcp_send_ack(sk);
    sk->sk_state = TCP_ESTABLISHED;
}

/* 服务端子连接收到 ACK → ESTABLISHED + 加入 accept 队列 */
void tcp_rcv_synrecv_state_process(struct sock *sk, struct sk_buff *skb) {
    sk->sk_state = TCP_ESTABLISHED;
    /* 加入监听 socket 的 accept 队列 */
}
```

## 11.5 netfilter钩子框架（参考 net/netfilter/core.c）

```c
/* 参考 include/linux/netfilter.h */

/* 5个IPv4 Hook点 */
enum nf_inet_hooks {
    NF_INET_PRE_ROUTING,    /* 收包，路由前 */
    NF_INET_LOCAL_IN,       /* 收包，目的是本机 */
    NF_INET_FORWARD,        /* 转发包 */
    NF_INET_LOCAL_OUT,      /* 本机发出的包 */
    NF_INET_POST_ROUTING,   /* 发包，离开前 */
};

/* Hook返回值 */
#define NF_DROP   0   /* 丢弃数据包 */
#define NF_ACCEPT 1   /* 继续处理 */
#define NF_STOLEN 2   /* 钩子接管，不再继续 */
#define NF_QUEUE  3   /* 送往用户态队列 */

/* 注册一个 netfilter 钩子 */
struct nf_hook_ops {
    nf_hookfn       *hook;          /* 钩子函数 */
    struct net_device *dev;
    void            *priv;
    u_int8_t        pf;             /* NFPROTO_IPV4 */
    unsigned int    hooknum;        /* NF_INET_* */
    int             priority;       /* 优先级（越小越先执行）*/
};

int nf_register_net_hook(struct net *net, const struct nf_hook_ops *ops) {
    struct nf_hook_entries *entries;
    /* 将 ops 插入对应 hook 点的有序链表 */
    nf_hook_entries_grow(entries, ops);
    rcu_assign_pointer(net->nf.hooks_ipv4[ops->hooknum], entries);
    return 0;
}

/* 遍历并执行某 hook 点的所有钩子 */
unsigned int nf_hook_slow(struct sk_buff *skb, struct nf_hook_state *state,
                          const struct nf_hook_entries *e) {
    unsigned int verdict = NF_ACCEPT;
    for (int i = 0; i < e->num_hook_entries; i++) {
        verdict = e->hooks[i].hook(e->hooks[i].priv, skb, state);
        if (verdict != NF_ACCEPT)
            break;
    }
    return verdict;
}
```

## 11.6 iptables/nftables规则匹配

```c
/* 简化的 iptables filter 规则（参考 net/ipv4/netfilter/iptable_filter.c）*/

/* 规则结构 */
struct ipt_entry {
    struct ipt_ip ip;       /* 匹配条件（src/dst IP、接口、协议）*/
    unsigned int  target_offset; /* target 在结构体内的偏移 */
    unsigned int  next_offset;   /* 下一条规则的偏移 */
    /* 后跟可选的 match 模块（端口匹配等）*/
    /* 后跟 target（ACCEPT/DROP/JUMP）*/
};

/* filter 表的 FORWARD 链钩子函数 */
unsigned int ipt_do_table(void *priv, struct sk_buff *skb,
                          const struct nf_hook_state *state) {
    struct ipt_entry *e = get_entry(table, 0);  /* 从第一条规则开始 */

    while (1) {
        /* 匹配 IP 头部 */
        if (ipt_match_ip(skb, &e->ip)) {
            /* 执行 target（ACCEPT/DROP/返回父链）*/
            int verdict = ipt_do_target(skb, e, state);
            if (verdict == NF_ACCEPT || verdict == NF_DROP)
                return verdict;
        }
        /* 移动到下一条规则 */
        e = ipt_next_entry(e);
        if (e == NULL) return NF_ACCEPT;  /* 默认策略 */
    }
}
```

## 11.7 容器网络：veth + bridge

```
容器网络拓扑（类似 Docker bridge 模式）：

宿主机 network namespace:
    br0 (bridge)
    ├── eth0 (物理网卡，对外)
    └── veth0 ──────────────────┐
                                │ veth pair（虚拟网线）
容器 network namespace:         │
    lo (127.0.0.1)              │
    veth1 ◄─────────────────────┘
    (172.17.0.2/16)

数据包流向（容器→外网）：
容器进程 → veth1 → veth0 → br0 → eth0 → 外网
                         ↑
              SNAT (NF_INET_POST_ROUTING)
              将 src 172.17.0.2 改为宿主机 IP
```

## 11.8 验证方法

```c
/*
 * 实际实现的三个测试：
 *   1. TCP loopback — 同步三次握手 + 数据收发
 *   2. netfilter hook — ICMP 丢弃钩子
 *   3. nftables rule — 端口匹配规则
 *
 * 注意：socket I/O 使用 sock_write/sock_read（非 sys_write/sys_read）
 * 所有操作在同一线程内同步完成（通过 loopback 队列驱动）
 */

void test_tcp(void) {
    /* 服务端 */
    int server_fd = sys_socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_port = htons(8080);
    addr.sin_addr.s_addr = INADDR_ANY;
    sys_bind(server_fd, &addr, sizeof(addr));
    sys_listen(server_fd, 5);

    /* 客户端（同一线程，loopback 同步驱动握手）*/
    int client_fd = sys_socket(AF_INET, SOCK_STREAM, 0);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    sys_connect(client_fd, &addr, sizeof(addr));

    /* 服务端 accept */
    int conn_fd = sys_accept(server_fd, NULL, NULL);

    /* 数据收发 */
    sock_write(conn_fd, "Hello TCP!\n", 11);

    char buf[64];
    sock_read(client_fd, buf, 11);
    boot_printk("TCP test OK: ");
    boot_printk(buf);
}

void test_netfilter_drop(void) {
    /* 注册钩子：丢弃所有 ICMP 包 */
    struct nf_hook_ops ops;
    ops.hook = drop_icmp_hook;    /* 检查 iph->protocol == IPPROTO_ICMP */
    ops.hooknum = NF_INET_LOCAL_IN;
    ops.priority = 0;
    nf_register_net_hook(&init_net, &ops);

    /* 构造测试 skb，验证 ICMP 被丢弃、TCP 被放行 */
    unsigned int verdict = nf_hook(NF_INET_LOCAL_IN, skb_icmp);
    /* verdict == NF_DROP */
}

void test_nftables_rule(void) {
    /* 添加 DROP 规则：目的端口 9999 */
    struct nft_rule rule;
    rule.dst_port = htons(9999);
    rule.target = NF_DROP;
    nft_add_rule(&nft_filter_table.input, &rule);

    /* 验证端口 9999 被 DROP，端口 80 被 ACCEPT（默认策略）*/
    nft_do_chain(&nft_filter_table.input, skb_9999);  /* → NF_DROP */
    nft_do_chain(&nft_filter_table.input, skb_80);    /* → NF_ACCEPT */
}
```

## 11.9 本阶段产出文件

```
arm64os/
├── include/linux/
│   ├── net.h               ← 网络核心数据结构（sock, iphdr, tcphdr, 字节序）
│   ├── skbuff.h            ← sk_buff 定义 + 内联操作（push/pull/reserve/put）
│   └── netfilter.h         ← netfilter hook 定义 + nft_rule/nft_chain/nft_table
├── include/linux/types.h   ← 新增 __be16/__be32/__wsum 网络字节序类型
├── include/linux/nsproxy.h ← 更新 struct net 加入 nf_hooks_ipv4[5]
└── net/
    ├── core/
    │   ├── skbuff.c        ← sk_buff 静态池分配（alloc_skb/kfree_skb）
    │   └── sock.c          ← socket 层（sys_socket/bind/listen/accept/connect + 查找）
    ├── ipv4/
    │   ├── af_inet.c       ← IPv4 协议族初始化（inet_init → ip_init + tcp_init）
    │   ├── ip_input.c      ← IP 收包（ip_rcv → ip_local_deliver → tcp_v4_rcv）
    │   ├── ip_output.c     ← IP 发包 + loopback 队列（ip_queue_xmit, net_rx_process）
    │   ├── tcp.c           ← TCP 主体（tcp_sendmsg/tcp_recvmsg/tcp_init）
    │   ├── tcp_input.c     ← TCP 收包状态机（listen/syn_sent/syn_recv/established）
    │   └── tcp_output.c    ← TCP 控制报文（SYN/SYN-ACK/ACK）+ tcp_v4_connect
    └── netfilter/
        ├── core.c          ← 钩子注册与执行框架（nf_hook/nf_hook_slow）
        └── nf_tables_core.c ← nftables 规则引擎（nft_do_chain/nft_add_rule）
```
