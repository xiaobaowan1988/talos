/* SPDX-License-Identifier: GPL-2.0 */
/*
 * arm64os/net/core/sock.c
 *
 * Socket 核心层 — socket 系统调用实现
 *
 * 参考：net/socket.c
 *       net/core/sock.c
 *       net/ipv4/af_inet.c
 *
 * Phase 11 实现：
 *   - sys_socket()：创建 TCP socket
 *   - sys_bind()：绑定地址和端口
 *   - sys_listen()：设置为监听模式
 *   - sys_accept()：接受连接
 *   - sys_connect()：发起 TCP 连接
 *   - sock_write() / sock_read()：socket 数据读写
 *
 * 简化说明：
 *   - 仅支持 AF_INET + SOCK_STREAM (TCP)
 *   - 静态 socket 池（MAX_SOCKETS 个）
 *   - socket fd 直接映射到池索引
 *   - 无 select/poll/epoll
 */

#include <linux/types.h>
#include <linux/net.h>
#include <linux/skbuff.h>

/* 外部函数声明 */
void boot_printk(const char *s);
void boot_printk_hex(unsigned long val);

/* TCP 层接口（net/ipv4/tcp*.c 提供）*/
int tcp_v4_connect(struct sock *sk, __be32 daddr, __be16 dport);
int tcp_sendmsg(struct sock *sk, const void *buf, size_t len);
int tcp_recvmsg(struct sock *sk, void *buf, size_t len);

/* ---- socket 静态池 ---- */
static struct sock sock_pool[MAX_SOCKETS];

/*
 * sock_init - 初始化 socket 池
 */
void sock_init(void)
{
    int i;
    for (i = 0; i < MAX_SOCKETS; i++) {
        sock_pool[i].used = 0;
        sock_pool[i].sk_state = TCP_CLOSE;
    }
    boot_printk("[net] socket layer initialized\n");
}

/*
 * sk_alloc - 从池中分配一个 sock
 *
 * 返回：分配的 sock 指针，或 NULL
 *
 * 参考：net/core/sock.c sk_alloc()
 */
struct sock *sk_alloc(void)
{
    int i;
    for (i = 0; i < MAX_SOCKETS; i++) {
        if (!sock_pool[i].used) {
            struct sock *sk = &sock_pool[i];
            int j;

            sk->used        = 1;
            sk->sk_state    = TCP_CLOSE;
            sk->sk_family   = AF_INET;
            sk->sk_type     = SOCK_STREAM;
            sk->sk_protocol = IPPROTO_TCP;
            sk->sk_saddr    = 0;
            sk->sk_daddr    = 0;
            sk->sk_sport    = 0;
            sk->sk_dport    = 0;
            sk->sk_bound    = 0;
            sk->snd_una     = 0;
            sk->snd_nxt     = 0;
            sk->rcv_nxt     = 0;
            sk->iss         = 0;
            sk->irs         = 0;
            sk->rx_head     = 0;
            sk->rx_tail     = 0;
            sk->accept_head = 0;
            sk->accept_tail = 0;
            sk->backlog     = 0;
            sk->sk_listener = NULL;

            for (j = 0; j < SOCK_ACCEPT_MAX; j++)
                sk->accept_queue[j] = NULL;

            return sk;
        }
    }
    return NULL;
}

/*
 * sk_free - 释放 sock
 */
void sk_free(struct sock *sk)
{
    if (sk)
        sk->used = 0;
}

/*
 * sock_get - 通过 socket 描述符获取 sock
 *
 * socket fd 直接对应池索引。
 */
struct sock *sock_get(int sockfd)
{
    if (sockfd < 0 || sockfd >= MAX_SOCKETS)
        return NULL;
    if (!sock_pool[sockfd].used)
        return NULL;
    return &sock_pool[sockfd];
}

/*
 * sock_fd - 获取 sock 的 socket 描述符（池索引）
 */
static int sock_fd(struct sock *sk)
{
    if (!sk)
        return -1;
    return (int)(sk - sock_pool);
}

/*
 * inet_lookup_listener - 查找监听 socket
 *
 * 按目的端口查找处于 LISTEN 状态的 socket。
 * 支持 INADDR_ANY（0.0.0.0）通配。
 *
 * 参考：net/ipv4/inet_hashtables.c inet_lookup_listener()
 */
struct sock *inet_lookup_listener(__be32 daddr, __be16 dport)
{
    int i;
    for (i = 0; i < MAX_SOCKETS; i++) {
        struct sock *sk = &sock_pool[i];
        if (!sk->used || sk->sk_state != TCP_LISTEN)
            continue;
        if (sk->sk_sport != dport)
            continue;
        /* INADDR_ANY 匹配任意目的地址 */
        if (sk->sk_saddr == 0 || sk->sk_saddr == daddr)
            return sk;
    }
    return NULL;
}

/*
 * inet_lookup_established - 查找已建立连接的 socket
 *
 * 按四元组（saddr, sport, daddr, dport）精确匹配。
 *
 * 参考：net/ipv4/inet_hashtables.c __inet_lookup_established()
 */
struct sock *inet_lookup_established(__be32 saddr, __be16 sport,
                                     __be32 daddr, __be16 dport)
{
    int i;
    for (i = 0; i < MAX_SOCKETS; i++) {
        struct sock *sk = &sock_pool[i];
        if (!sk->used)
            continue;
        if (sk->sk_state != TCP_ESTABLISHED &&
            sk->sk_state != TCP_SYN_RECV &&
            sk->sk_state != TCP_SYN_SENT)
            continue;
        /*
         * 注意：对于被查找的 socket，
         * 收到的包的 (saddr, sport) 是对端地址 = sk 的 (daddr, dport)
         * 收到的包的 (daddr, dport) 是本端地址 = sk 的 (saddr, sport)
         */
        if (sk->sk_saddr == daddr && sk->sk_sport == dport &&
            sk->sk_daddr == saddr && sk->sk_dport == sport)
            return sk;
    }
    return NULL;
}

/*
 * ============================================================
 * Socket 系统调用实现
 * ============================================================
 */

/*
 * sys_socket - 创建 socket
 *
 * 参考：net/socket.c __sys_socket()
 *
 * @family:   AF_INET
 * @type:     SOCK_STREAM
 * @protocol: 0 或 IPPROTO_TCP
 *
 * 返回：socket 描述符（>= 0），或 -1 失败
 */
int sys_socket(int family, int type, int protocol)
{
    struct sock *sk;

    if (family != AF_INET || type != SOCK_STREAM)
        return -1;

    sk = sk_alloc();
    if (!sk)
        return -1;

    sk->sk_state = TCP_CLOSE;
    return sock_fd(sk);
}

/*
 * sys_bind - 绑定地址和端口
 *
 * 参考：net/ipv4/af_inet.c inet_bind()
 */
int sys_bind(int sockfd, const struct sockaddr_in *addr, int addrlen)
{
    struct sock *sk = sock_get(sockfd);
    if (!sk || !addr)
        return -1;

    sk->sk_saddr = addr->sin_addr.s_addr;
    sk->sk_sport = addr->sin_port;
    sk->sk_bound = 1;
    return 0;
}

/*
 * sys_listen - 设置为监听模式
 *
 * 参考：net/ipv4/af_inet.c inet_listen()
 */
int sys_listen(int sockfd, int backlog)
{
    struct sock *sk = sock_get(sockfd);
    if (!sk)
        return -1;
    if (sk->sk_state != TCP_CLOSE)
        return -1;
    if (!sk->sk_bound)
        return -1;

    sk->sk_state = TCP_LISTEN;
    sk->backlog = (backlog > SOCK_ACCEPT_MAX) ? SOCK_ACCEPT_MAX : backlog;
    return 0;
}

/*
 * sys_accept - 接受连接
 *
 * 从 accept 队列中取出一个已完成三次握手的连接。
 *
 * 参考：net/ipv4/af_inet.c inet_accept()
 *
 * 返回：新 socket 描述符，或 -1 失败
 */
int sys_accept(int sockfd, struct sockaddr_in *addr, int *addrlen)
{
    struct sock *sk = sock_get(sockfd);
    struct sock *child;

    if (!sk || sk->sk_state != TCP_LISTEN)
        return -1;

    /* 检查 accept 队列 */
    if (sk->accept_head == sk->accept_tail)
        return -1;  /* 无就绪连接 */

    child = sk->accept_queue[sk->accept_head % SOCK_ACCEPT_MAX];
    sk->accept_head++;

    if (!child)
        return -1;

    /* 填充对端地址（如果请求）*/
    if (addr) {
        addr->sin_family = AF_INET;
        addr->sin_port = child->sk_dport;
        addr->sin_addr.s_addr = child->sk_daddr;
    }

    return sock_fd(child);
}

/*
 * sys_connect - 发起 TCP 连接
 *
 * 参考：net/ipv4/af_inet.c inet_stream_connect()
 *
 * 调用 TCP 层执行三次握手（通过 loopback 同步完成）。
 */
int sys_connect(int sockfd, const struct sockaddr_in *addr, int addrlen)
{
    struct sock *sk = sock_get(sockfd);
    if (!sk || !addr)
        return -1;
    if (sk->sk_state != TCP_CLOSE)
        return -1;

    return tcp_v4_connect(sk, addr->sin_addr.s_addr, addr->sin_port);
}

/*
 * sock_write - 通过 socket 发送数据
 *
 * 参考：net/socket.c sock_write_iter → tcp_sendmsg
 */
ssize_t sock_write(int sockfd, const void *buf, size_t len)
{
    struct sock *sk = sock_get(sockfd);
    if (!sk)
        return -1;
    if (sk->sk_state != TCP_ESTABLISHED)
        return -1;

    return (ssize_t)tcp_sendmsg(sk, buf, len);
}

/*
 * sock_read - 从 socket 接收数据
 *
 * 参考：net/socket.c sock_read_iter → tcp_recvmsg
 */
ssize_t sock_read(int sockfd, void *buf, size_t len)
{
    struct sock *sk = sock_get(sockfd);
    if (!sk)
        return -1;
    if (sk->sk_state != TCP_ESTABLISHED)
        return -1;

    return (ssize_t)tcp_recvmsg(sk, buf, len);
}
