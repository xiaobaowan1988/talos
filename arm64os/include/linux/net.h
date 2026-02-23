/* SPDX-License-Identifier: GPL-2.0 */
/*
 * arm64os/include/linux/net.h
 *
 * 网络子系统核心数据结构
 *
 * 参考：include/linux/net.h
 *       include/uapi/linux/in.h
 *       include/uapi/linux/ip.h
 *       include/uapi/linux/tcp.h
 *       include/linux/netdevice.h
 *
 * Phase 11 实现：
 *   - struct sockaddr_in：IPv4 套接字地址
 *   - struct sock：网络 socket 状态（TCP 状态机、地址、端口）
 *   - struct iphdr：IP 头部
 *   - struct tcphdr：TCP 头部
 *   - 字节序转换：htons/ntohs/htonl/ntohl
 *   - Socket 系统调用接口
 *
 * 简化说明：
 *   - 仅支持 IPv4 (AF_INET) + TCP (SOCK_STREAM)
 *   - 无 UDP、ICMP 完整实现
 *   - 无 SO_* 选项
 *   - 静态池分配
 */

#ifndef __LINUX_NET_H
#define __LINUX_NET_H

#include <linux/types.h>

/* 网络字节序类型已在 types.h 中定义：__be16, __be32, __wsum */

/*
 * ============================================================
 * 字节序转换（ARM64 小端 ↔ 网络大端）
 *
 * 参考：include/uapi/linux/byteorder/little_endian.h
 * ============================================================
 */
static inline __be16 htons(u16 hostshort)
{
    return (__be16)__builtin_bswap16(hostshort);
}

static inline u16 ntohs(__be16 netshort)
{
    return (u16)__builtin_bswap16(netshort);
}

static inline __be32 htonl(u32 hostlong)
{
    return (__be32)__builtin_bswap32(hostlong);
}

static inline u32 ntohl(__be32 netlong)
{
    return (u32)__builtin_bswap32(netlong);
}

/*
 * ============================================================
 * 地址族、协议、Socket 类型
 *
 * 参考：include/linux/socket.h, include/uapi/linux/in.h
 * ============================================================
 */
#define AF_INET         2           /* IPv4 */

#define SOCK_STREAM     1           /* TCP 面向连接 */
#define SOCK_DGRAM      2           /* UDP 无连接 */

#define IPPROTO_ICMP    1
#define IPPROTO_TCP     6
#define IPPROTO_UDP     17

/* 特殊地址 */
#define INADDR_ANY      0x00000000U     /* 0.0.0.0 */
#define INADDR_LOOPBACK 0x7f000001U     /* 127.0.0.1 */

/* 以太网协议类型 */
#define ETH_P_IP        0x0800

/*
 * ============================================================
 * TCP 状态（参考 include/net/tcp_states.h）
 * ============================================================
 */
enum tcp_state {
    TCP_CLOSE = 0,
    TCP_LISTEN,
    TCP_SYN_SENT,
    TCP_SYN_RECV,
    TCP_ESTABLISHED,
    TCP_FIN_WAIT1,
    TCP_FIN_WAIT2,
    TCP_TIME_WAIT,
    TCP_CLOSE_WAIT,
    TCP_LAST_ACK,
    TCP_CLOSING,
};

/* TCP 头部标志位 */
#define TCPHDR_FIN      0x01
#define TCPHDR_SYN      0x02
#define TCPHDR_RST      0x04
#define TCPHDR_PSH      0x08
#define TCPHDR_ACK      0x10
#define TCPHDR_URG      0x20

/*
 * ============================================================
 * IPv4 地址结构
 *
 * 参考：include/uapi/linux/in.h
 * ============================================================
 */
struct in_addr {
    __be32  s_addr;             /* 网络字节序 IP 地址 */
};

struct sockaddr_in {
    u16             sin_family;     /* AF_INET */
    __be16          sin_port;       /* 端口号（网络字节序）*/
    struct in_addr  sin_addr;       /* IP 地址 */
    u8              sin_zero[8];    /* 填充到 16 字节 */
};

/*
 * ============================================================
 * IP 头部（20 字节，无选项）
 *
 * 参考：include/uapi/linux/ip.h struct iphdr
 * ============================================================
 */
struct iphdr {
    u8      version_ihl;    /* 版本（高4位）+ 首部长度（低4位，单位4字节）*/
    u8      tos;            /* 服务类型 */
    __be16  tot_len;        /* 总长度（含头部+数据）*/
    __be16  id;             /* 标识（分片重组用）*/
    __be16  frag_off;       /* 标志+片偏移 */
    u8      ttl;            /* 生存时间 */
    u8      protocol;       /* 上层协议（IPPROTO_TCP 等）*/
    __be16  check;          /* 首部校验和 */
    __be32  saddr;          /* 源 IP 地址 */
    __be32  daddr;          /* 目的 IP 地址 */
};

/* IP 头部固定长度（无选项）*/
#define IP_HDR_LEN      20

/*
 * ============================================================
 * TCP 头部（20 字节，无选项）
 *
 * 参考：include/uapi/linux/tcp.h struct tcphdr
 * ============================================================
 */
struct tcphdr {
    __be16  source;         /* 源端口 */
    __be16  dest;           /* 目的端口 */
    __be32  seq;            /* 序列号 */
    __be32  ack_seq;        /* 确认号 */
    u8      doff_res;       /* 数据偏移（高4位，单位4字节）+ 保留（低4位）*/
    u8      flags;          /* TCP 标志（FIN/SYN/RST/PSH/ACK/URG）*/
    __be16  window;         /* 窗口大小 */
    __be16  check;          /* 校验和 */
    __be16  urg_ptr;        /* 紧急指针 */
};

/* TCP 头部固定长度（无选项）*/
#define TCP_HDR_LEN     20

/*
 * ============================================================
 * Socket 内核对象（参考 include/net/sock.h struct sock）
 *
 * 简化版：仅支持 TCP，静态分配，无引用计数。
 * ============================================================
 */
#define MAX_SOCKETS         16
#define SOCK_RX_BUF_SIZE    4096
#define SOCK_ACCEPT_MAX     8

struct sk_buff;  /* 前向声明 */

struct sock {
    /* 状态 */
    int             sk_state;       /* TCP 状态（enum tcp_state）*/
    int             sk_family;      /* AF_INET */
    int             sk_type;        /* SOCK_STREAM */
    int             sk_protocol;    /* IPPROTO_TCP */

    /* 地址 */
    __be32          sk_saddr;       /* 本地 IP 地址（网络字节序）*/
    __be32          sk_daddr;       /* 远端 IP 地址（网络字节序）*/
    __be16          sk_sport;       /* 本地端口（网络字节序）*/
    __be16          sk_dport;       /* 远端端口（网络字节序）*/
    int             sk_bound;       /* 已绑定标志 */

    /* TCP 序列号 */
    u32             snd_una;        /* 最早未确认的发送序列号 */
    u32             snd_nxt;        /* 下一个发送序列号 */
    u32             rcv_nxt;        /* 期望接收的下一个序列号 */
    u32             iss;            /* 初始发送序列号 */
    u32             irs;            /* 初始接收序列号 */

    /* 接收缓冲区（简化的环形队列）*/
    unsigned char   rx_buf[SOCK_RX_BUF_SIZE];
    int             rx_head;
    int             rx_tail;

    /* 监听 socket 的 accept 队列 */
    struct sock    *accept_queue[SOCK_ACCEPT_MAX];
    int             accept_head;
    int             accept_tail;
    int             backlog;        /* 最大 accept 队列长度 */

    /* 关联 */
    struct sock    *sk_listener;    /* 监听方父 socket（对 SYN_RECV 子连接有效）*/
    int             used;           /* 池分配标记 */
};

/*
 * ============================================================
 * Socket 系统调用接口（net/core/sock.c 提供）
 *
 * 参考：net/socket.c, include/linux/syscalls.h
 * ============================================================
 */

/* 创建 socket，返回 socket 描述符 */
int sys_socket(int family, int type, int protocol);

/* 绑定地址 */
int sys_bind(int sockfd, const struct sockaddr_in *addr, int addrlen);

/* 设置为监听模式 */
int sys_listen(int sockfd, int backlog);

/* 接受连接，返回新 socket 描述符 */
int sys_accept(int sockfd, struct sockaddr_in *addr, int *addrlen);

/* 发起连接 */
int sys_connect(int sockfd, const struct sockaddr_in *addr, int addrlen);

/* socket 读写 */
ssize_t sock_write(int sockfd, const void *buf, size_t len);
ssize_t sock_read(int sockfd, void *buf, size_t len);

/*
 * ============================================================
 * 内部 socket 层接口
 * ============================================================
 */

/* sock 池操作 */
struct sock *sk_alloc(void);
void sk_free(struct sock *sk);
struct sock *sock_get(int sockfd);

/* 通过地址查找 socket */
struct sock *inet_lookup_listener(__be32 daddr, __be16 dport);
struct sock *inet_lookup_established(__be32 saddr, __be16 sport,
                                     __be32 daddr, __be16 dport);

/* socket 层初始化 */
void sock_init(void);

/*
 * ============================================================
 * 网络子系统总初始化入口
 * ============================================================
 */
void net_init(void);

/* loopback 接收队列处理 */
void net_rx_process(void);

#endif /* __LINUX_NET_H */
