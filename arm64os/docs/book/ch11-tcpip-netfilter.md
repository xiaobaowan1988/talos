# Phase 11：TCP/IP 协议栈 + netfilter 框架

## 知识来源总览

- **RFC 793（TCP）+ RFC 791（IP）**：约 30%（三次握手状态机、IP 报头格式）
- **Linux 网络栈架构**：约 25%（sk_buff 设计、NAPI 软中断收包）
- **netfilter 框架**：约 20%（5 个 hook 点、优先级链、iptables 规则匹配）
- **容器网络模型**：约 15%（veth pair、bridge、SNAT）
- **前序依赖**：约 10%

## sk_buff：网络包的通用容器

```c
struct sk_buff {
    unsigned char   *head;   /* 缓冲区起始（固定）*/
    unsigned char   *data;   /* 当前有效数据起始（随协议层移动）*/
    unsigned char   *tail;   /* 当前有效数据结束 */
    unsigned char   *end;    /* 缓冲区结束（固定）*/

    struct sock     *sk;
    struct net_device *dev;
    __be16          protocol;
    unsigned int    len;
    unsigned int    data_len;
};
```

**四指针设计的精髓**：`head` 和 `end` 是固定的缓冲区边界，`data` 和 `tail` 是当前有效数据的边界。协议栈各层通过移动 `data` 指针来"添加"或"去除"协议头部，而不需要复制数据。

**为什么是四个而不是两个？** 如果只有 data 和 len，添加头部时需要 memmove 整个数据区。四指针设计让这变成一个简单的指针减法：

```c
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

**发送方向**（用户数据 → 网线）：先 alloc_skb 预留足够 headroom，然后 TCP 层 `skb_push(tcphdr)`，IP 层 `skb_push(iphdr)`，以太网层 `skb_push(ethhdr)`——每层都在 data 前面添加自己的头部。

**接收方向**（网线 → 用户数据）：以太网层 `skb_pull(ethhdr)`，IP 层 `skb_pull(iphdr)`，TCP 层 `skb_pull(tcphdr)`——每层剥掉自己的头部，把 data 指向下一层的 payload。

整个过程零拷贝。一个数据包从 NIC 到用户态，数据本身始终在同一块内存中，只是 data 指针在移动。

## 网络数据包的完整生命周期

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
```

**NAPI（New API）收包模型**：NIC 收到第一个包时触发硬中断，然后切换到 poll 模式（软中断中循环收取），直到队列为空才重新启用硬中断。这避免了高流量时的中断风暴。

**路由决策**是 IP 层的核心分叉点：`ip_rcv()` 查完路由表后，本机目的走 `ip_local_deliver()` → TCP/UDP 处理，非本机目的走 `ip_forward()` → 转发。

## Socket 系统调用层

```c
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
```

**为什么 socket 返回文件描述符？** UNIX "一切皆文件" 哲学。socket 通过 `sock_alloc_file()` 包装成 `struct file`，安装到进程的 fd 表中。之后 `read()/write()` 直接通过 VFS 分发到 socket 的操作函数——Phase 7 中 VFS 四大对象的 `file_operations` 在这里发挥作用。

**`net_families[family]`**：协议族注册表。`AF_INET`（IPv4）、`AF_INET6`（IPv6）、`AF_UNIX`（本地套接字）各自注册自己的 `create` 方法。这是内核网络栈的多态设计。

```c
int inet_create(struct socket *sock, int protocol) {
    struct sock *sk;
    struct proto *prot = &tcp_prot;

    /* 分配 tcp_sock（包含 sock + inet_sock + tcp_sock 三层）*/
    sk = sk_alloc(net, AF_INET, GFP_KERNEL, prot);
    sock_init_data(sock, sk);

    sk->sk_state = TCP_CLOSE;
    return 0;
}
```

**三层结构**：`struct sock`（传输层通用）→ `struct inet_sock`（IPv4 特有字段）→ `struct tcp_sock`（TCP 特有字段）。C 语言通过把基础结构体放在派生结构体的第一个字段来实现"继承"。`sk_alloc()` 根据 `prot->obj_size` 分配正确大小的内存。

**初始状态 `TCP_CLOSE`**：对应 RFC 793 状态机的 CLOSED 状态。从这里开始，`connect()` 走向 SYN_SENT，`listen()` 走向 LISTEN。

## TCP 三次握手

```c
/* 客户端 connect() → 发送 SYN */
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
```

**`tp->write_seq++`**：TCP 的 Initial Sequence Number（ISN）。RFC 793 要求 ISN 不能是固定值（否则有安全风险），Linux 使用 `secure_tcp_seq()` 基于源/目的 IP+端口+时间生成。SYN 占用一个序列号空间（即使没有数据），所以 `write_seq++`。

**状态转换 `TCP_CLOSE → TCP_SYN_SENT`**：严格遵循 RFC 793 的状态机。`tcp_set_state()` 不仅改变状态值，还更新各种统计计数器和定时器。

```c
/* 客户端收到 SYN-ACK，发送最终 ACK */
int tcp_rcv_synsent_state_process(struct sock *sk, struct sk_buff *skb) {
    struct tcphdr *th = tcp_hdr(skb);

    if (th->syn && th->ack) {
        tp->rcv_nxt = TCP_SKB_CB(skb)->seq + 1;
        tp->snd_una = TCP_SKB_CB(skb)->ack_seq;

        tcp_send_ack(sk);
        tcp_set_state(sk, TCP_ESTABLISHED);
    }
    return 0;
}
```

**`rcv_nxt = seq + 1`**：SYN 占一个序列号，所以期望下一个收到的字节序号是 `SYN.seq + 1`。

**`snd_una = ack_seq`**：`una` = unacknowledged。对方确认到 `ack_seq`，意味着之前发送的所有数据（包括 SYN）都已被确认。

**三次握手的完整状态转换**：

```
客户端                          服务端
CLOSED                          LISTEN
  │                               │
  ├─── SYN (seq=x) ──────────→   │
  │    状态 → SYN_SENT            │
  │                               ├─── 状态 → SYN_RCVD
  │   ←──── SYN-ACK (seq=y,      │
  │          ack=x+1) ───────────┘
  ├─── 状态 → ESTABLISHED
  │
  ├─── ACK (ack=y+1) ────────→   │
  │                               ├─── 状态 → ESTABLISHED
```

## netfilter 钩子框架

```c
enum nf_inet_hooks {
    NF_INET_PRE_ROUTING,    /* 收包，路由前 */
    NF_INET_LOCAL_IN,       /* 收包，目的是本机 */
    NF_INET_FORWARD,        /* 转发包 */
    NF_INET_LOCAL_OUT,      /* 本机发出的包 */
    NF_INET_POST_ROUTING,   /* 发包，离开前 */
};
```

**五个 hook 点的位置选择**：不是随意的，而是覆盖了数据包路径上的所有关键决策点。PRE_ROUTING 在路由查找之前（可以做 DNAT 改变目的地），POST_ROUTING 在离开网卡之前（可以做 SNAT 改变源地址）。LOCAL_IN/LOCAL_OUT 分别守护本机的收发。FORWARD 处理转发流量。

```c
struct nf_hook_ops {
    nf_hookfn       *hook;      /* 钩子函数 */
    u_int8_t        pf;         /* NFPROTO_IPV4 */
    unsigned int    hooknum;    /* NF_INET_* */
    int             priority;   /* 优先级（越小越先执行）*/
};
```

**优先级链**：同一 hook 点可以注册多个钩子，按 `priority` 从小到大依次执行。Linux 定义了标准优先级常量：`NF_IP_PRI_CONNTRACK(-200)` < `NF_IP_PRI_MANGLE(-150)` < `NF_IP_PRI_NAT_DST(-100)` < `NF_IP_PRI_FILTER(0)` < `NF_IP_PRI_NAT_SRC(100)`。conntrack 先于 filter，因为有状态匹配需要先建立连接跟踪。

```c
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

**短路求值**：任何一个钩子返回 `NF_DROP`，后续钩子不再执行，数据包直接丢弃。这就是防火墙规则的执行语义——第一条匹配的规则决定命运。

## iptables 规则匹配

```c
struct ipt_entry {
    struct ipt_ip ip;            /* 匹配条件（src/dst IP、接口、协议）*/
    unsigned int  target_offset; /* target 在结构体内的偏移 */
    unsigned int  next_offset;   /* 下一条规则的偏移 */
};

unsigned int ipt_do_table(void *priv, struct sk_buff *skb,
                          const struct nf_hook_state *state) {
    struct ipt_entry *e = get_entry(table, 0);

    while (1) {
        if (ipt_match_ip(skb, &e->ip)) {
            int verdict = ipt_do_target(skb, e, state);
            if (verdict == NF_ACCEPT || verdict == NF_DROP)
                return verdict;
        }
        e = ipt_next_entry(e);
        if (e == NULL) return NF_ACCEPT;  /* 默认策略 */
    }
}
```

**规则在内存中的布局**：不是链表，而是连续内存块。`next_offset` 是下一条规则相对当前规则起始的字节偏移。连续内存对 CPU 缓存友好，遍历规则链时几乎不会 cache miss。

**target_offset**：每条规则由可变长的 match 模块（端口匹配、状态匹配等）和固定的 target（ACCEPT/DROP/JUMP）组成。target 在规则结构体内的位置由 `target_offset` 指定。

**默认策略 `NF_ACCEPT`**：所有规则都不匹配时的行为。生产环境中通常设置默认策略为 DROP（白名单模式）。

## 容器网络：veth + bridge

```
宿主机 network namespace:
    br0 (bridge)
    ├── eth0 (物理网卡，对外)
    └── veth0 ──────────────────┐
                                │ veth pair（虚拟网线）
容器 network namespace:         │
    lo (127.0.0.1)              │
    veth1 ◄─────────────────────┘
    (172.17.0.2/16)
```

**veth pair**：一对虚拟网络设备，像一根网线的两端。往一端写入的数据从另一端读出。创建时用 `ip link add veth0 type veth peer name veth1`，然后把 veth1 移入容器的 network namespace。

**bridge（网桥）**：工作在数据链路层（L2），像一台虚拟交换机。连接到 br0 的所有设备在同一个广播域中。宿主机上的 br0 连接物理网卡和所有容器的 veth 端。

**SNAT 在 POST_ROUTING**：容器发出的包源地址是 172.17.0.2（私网地址），必须在离开宿主机前被替换为宿主机的公网 IP。这由注册在 `NF_INET_POST_ROUTING` 的 NAT 钩子完成。对应 iptables 规则：`iptables -t nat -A POSTROUTING -s 172.17.0.0/16 -o eth0 -j MASQUERADE`。

**与 Phase 10 的联动**：network namespace（Phase 10）提供网络隔离，veth + bridge 提供容器间通信和外网访问，netfilter 提供流量控制和 NAT。三者组合构成完整的容器网络方案。
