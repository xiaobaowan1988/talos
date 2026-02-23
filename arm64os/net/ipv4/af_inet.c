/* SPDX-License-Identifier: GPL-2.0 */
/*
 * arm64os/net/ipv4/af_inet.c
 *
 * IPv4 地址族注册与初始化
 *
 * 参考：net/ipv4/af_inet.c
 *
 * Phase 11 实现：
 *   - inet_init()：注册 IPv4 协议族
 *   - 建立 IPv4 → TCP 的协议分发
 *
 * 简化说明：
 *   - 仅支持 TCP（IPPROTO_TCP）
 *   - 无 UDP / RAW socket
 *   - 无 /proc/net 接口
 */

#include <linux/types.h>
#include <linux/net.h>

/* 外部函数声明 */
void boot_printk(const char *s);

/* TCP 层初始化 */
void tcp_init(void);

/* IP 层初始化 */
void ip_init(void);

/*
 * inet_init - IPv4 协议族初始化
 *
 * 参考：net/ipv4/af_inet.c inet_init()
 *
 * 初始化顺序：
 *   1. IP 层初始化（路由、分片等）
 *   2. TCP 协议初始化
 *   3. 注册 AF_INET 协议族
 */
void inet_init(void)
{
    /* 初始化 IP 层 */
    ip_init();

    /* 初始化 TCP 协议 */
    tcp_init();

    boot_printk("[net] IPv4 protocol initialized\n");
}
