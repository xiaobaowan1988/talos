# Phase 11：TCP/IP协议栈 + netfilter框架

## 参考内核文件

```
net/core/sock.c             # socket核心（sk_buff、socket创建）
net/core/skbuff.c           # sk_buff（网络数据包缓冲区）
net/ipv4/af_inet.c          # IPv4地址族（socket系统调用接入）
net/ipv4/ip_input.c         # IP收包处理（ip_rcv）
net/ipv4/ip_output.c        # IP发包处理（ip_queue_xmit）
net/ipv4/tcp.c              # TCP协议主体
net/ipv4/tcp_input.c        # TCP收包（三次握手、数据接收）
net/ipv4/tcp_output.c       # TCP发包（拥塞控制、重传）
net/ipv4/tcp_ipv4.c         # TCP over IPv4绑定
net/netfilter/core.c        # netfilter钩子框架
net/netfilter/nf_tables_core.c  # nftables规则引擎
net/ipv4/netfilter/iptable_filter.c  # iptables filter表
include/linux/skbuff.h      # sk_buff数据结构
include/linux/netfilter.h   # netfilter钩子定义
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

struct sk_buff {
    /* 包数据指针 */
    unsigned char   *head;   /* 缓冲区起始（固定）*/
    unsigned char   *data;   /* 当前有效数据起始（随协议层移动）*/
    unsigned char   *tail;   /* 当前有效数据结束 */
    unsigned char   *end;    /* 缓冲区结束（固定）*/

    /* 元数据 */
    struct sock     *sk;        /* 关联的socket */
    struct net_device *dev;     /* 网络设备 */
    __be16          protocol;   /* ETH_P_IP, ETH_P_ARP 等 */
    unsigned int    len;        /* 数据总长度 */
    unsigned int    data_len;   /* 分片数据长度 */

    /* 校验和 */
    __wsum          csum;
    __u8            ip_summed;

    /* 路由信息 */
    struct dst_entry *dst;

    /* 协议头指针（网络层处理时设置）*/
    union {
        struct iphdr    *iph;
        struct ipv6hdr  *ipv6h;
    } network_header;
    union {
        struct tcphdr   *th;
        struct udphdr   *uh;
    } transport_header;
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
/* socket(AF_INET, SOCK_STREAM, IPPROTO_TCP) 调用链 */
int __sys_socket(int family, int type, int protocol) {
    struct socket *sock;

    /* 1. 创建 socket 对象 */
    sock = sock_alloc();
    sock->type = type;

    /* 2. 调用协议族的 create 方法 */
    /* family=AF_INET → inet_family_ops.create = inet_create */
    net_families[family]->create(net, sock, protocol);

    /* 3. 安装到文件描述符 */
    int fd = get_unused_fd_flags(0);
    struct file *file = sock_alloc_file(sock);
    fd_install(fd, file);
    return fd;
}

/* inet_create：创建 TCP socket */
int inet_create(struct socket *sock, int protocol) {
    struct sock *sk;

    /* 查找协议：IPPROTO_TCP → tcp_prot */
    struct proto *prot = &tcp_prot;

    /* 分配 tcp_sock（包含 sock + inet_sock + tcp_sock 三层）*/
    sk = sk_alloc(net, AF_INET, GFP_KERNEL, prot);
    sock_init_data(sock, sk);

    /* TCP 特有初始化 */
    sk->sk_state = TCP_CLOSE;
    inet->inet_num = 0;
    return 0;
}
```

## 11.4 TCP三次握手（参考 tcp_input.c + tcp_output.c）

```c
/* 服务端：listen() + accept() */

/* 客户端 connect() 发送 SYN */
int tcp_v4_connect(struct sock *sk, struct sockaddr *uaddr) {
    /* 选择源端口（如果未bind）*/
    inet_hash_connect(&tcp_death_row, sk);

    /* 发送 SYN 包 */
    tcp_connect(sk);
    return 0;
}

int tcp_connect(struct sock *sk) {
    struct sk_buff *buff = sk_stream_alloc_skb(sk, 0, sk->sk_allocation);

    /* 构造 SYN 报文 */
    tcp_init_nondata_skb(buff, tp->write_seq++, TCPHDR_SYN);

    /* 设置状态：CLOSE → SYN_SENT */
    tcp_set_state(sk, TCP_SYN_SENT);

    /* 发送 */
    tcp_transmit_skb(sk, buff, 1, sk->sk_allocation);
    return 0;
}

/* 服务端收到 SYN，发送 SYN-ACK（参考 tcp_input.c tcp_rcv_state_process）*/
int tcp_rcv_synsent_state_process(struct sock *sk, struct sk_buff *skb) {
    struct tcphdr *th = tcp_hdr(skb);

    if (th->syn && th->ack) {
        /* 收到 SYN-ACK，更新序列号，发送 ACK */
        tp->rcv_nxt = TCP_SKB_CB(skb)->seq + 1;
        tp->snd_una = TCP_SKB_CB(skb)->ack_seq;

        tcp_send_ack(sk);           /* 发送最后的 ACK */
        tcp_set_state(sk, TCP_ESTABLISHED);
    }
    return 0;
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
void test_tcp(void) {
    /* 服务端 */
    int server_fd = sys_socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(8080),
        .sin_addr.s_addr = INADDR_ANY,
    };
    sys_bind(server_fd, &addr, sizeof(addr));
    sys_listen(server_fd, 5);

    /* 客户端（另一个线程/进程）*/
    int client_fd = sys_socket(AF_INET, SOCK_STREAM, 0);
    addr.sin_addr.s_addr = htonl(0x7f000001);  /* 127.0.0.1 */
    sys_connect(client_fd, &addr, sizeof(addr));

    int conn_fd = sys_accept(server_fd, NULL, NULL);
    sys_write(conn_fd, "Hello TCP!\n", 11);

    char buf[16];
    sys_read(client_fd, buf, 11);
    printk("TCP test OK: %s\n", buf);
}

void test_netfilter_drop(void) {
    /* 注册钩子：丢弃所有 ICMP 包 */
    struct nf_hook_ops ops = {
        .hook = drop_icmp_hook,
        .pf   = NFPROTO_IPV4,
        .hooknum = NF_INET_LOCAL_IN,
        .priority = NF_IP_PRI_FIRST,
    };
    nf_register_net_hook(&init_net, &ops);
    printk("ICMP drop rule installed\n");
}
```

## 11.9 本阶段产出文件

```
arm64os/
└── net/
    ├── core/
    │   ├── sock.c          ← socket核心、sk_buff分配
    │   └── skbuff.c        ← sk_buff操作
    ├── ipv4/
    │   ├── af_inet.c       ← IPv4地址族注册
    │   ├── ip_input.c      ← IP收包（核心）
    │   ├── ip_output.c     ← IP发包
    │   ├── tcp.c           ← TCP主体（核心）
    │   ├── tcp_input.c     ← TCP收包与状态机
    │   └── tcp_output.c    ← TCP发包与拥塞控制
    └── netfilter/
        ├── core.c          ← 钩子注册与执行框架（核心）
        └── nf_tables_core.c ← nftables规则引擎
```
