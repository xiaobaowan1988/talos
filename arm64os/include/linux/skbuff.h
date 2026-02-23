/* SPDX-License-Identifier: GPL-2.0 */
/*
 * arm64os/include/linux/skbuff.h
 *
 * sk_buff — 网络数据包缓冲区
 *
 * 参考：include/linux/skbuff.h
 *
 * Phase 11 实现：
 *   - sk_buff 是内核网络栈中最核心的数据结构
 *   - 四个关键指针：head/data/tail/end
 *     head/end 固定（缓冲区边界），data/tail 随协议层移动
 *   - 各层通过 skb_push（添加头部）/ skb_pull（去除头部）操作
 *
 * 简化说明：
 *   - 内联数据缓冲区（无分散/聚集 I/O）
 *   - 无 SKB 共享（无 skb_clone / skb_shared_info）
 *   - 静态池分配
 */

#ifndef __LINUX_SKBUFF_H
#define __LINUX_SKBUFF_H

#include <linux/types.h>

/* 前向声明 */
struct sock;
struct iphdr;
struct tcphdr;

/* 网络字节序类型已在 types.h 中定义：__be16, __be32, __wsum */

/* SKB 数据缓冲区大小（足以容纳 IP + TCP 头 + MTU 数据）*/
#define SKB_DATA_SIZE       2048

/* 静态池大小 */
#define MAX_SKBS            64

/*
 * ============================================================
 * struct sk_buff — 网络数据包缓冲区
 *
 * 参考：include/linux/skbuff.h struct sk_buff
 *
 * 数据布局：
 *   head ──────── 缓冲区起始（固定）
 *   [headroom]    预留空间（skb_reserve 设置）
 *   data ──────── 当前有效数据起始（随协议层移动）
 *   [payload]     IP头 + TCP头 + 应用数据
 *   tail ──────── 当前有效数据结束
 *   [tailroom]    剩余空间
 *   end  ──────── 缓冲区结束（固定）
 *
 * 发送路径：
 *   TCP: skb_reserve(headroom) → skb_put(data) → skb_push(TCP头) → skb_push(IP头)
 *
 * 接收路径：
 *   NIC: 整个帧放入 skb → skb_pull(IP头) → skb_pull(TCP头) → 剩下 payload
 * ============================================================
 */
struct sk_buff {
    /* 包数据指针 */
    unsigned char   *head;      /* 缓冲区起始（固定）*/
    unsigned char   *data;      /* 当前有效数据起始（随协议层移动）*/
    unsigned char   *tail;      /* 当前有效数据结束 */
    unsigned char   *end;       /* 缓冲区结束（固定）*/

    /* 元数据 */
    struct sock     *sk;        /* 关联的 socket */
    __be16          protocol;   /* ETH_P_IP, ETH_P_ARP 等 */
    unsigned int    len;        /* 数据总长度（data 到 tail）*/

    /* 协议头指针（各层处理时设置）*/
    struct iphdr    *nh;        /* 网络层头部（IP）*/
    struct tcphdr   *th;        /* 传输层头部（TCP）*/

    /* 池管理 */
    int             used;       /* 是否已分配 */

    /* 内联数据缓冲区 */
    unsigned char   buf[SKB_DATA_SIZE];
};

/*
 * ============================================================
 * sk_buff 操作函数
 * ============================================================
 */

/*
 * alloc_skb - 分配一个 sk_buff
 *
 * @size: 需要的数据空间大小（需 <= SKB_DATA_SIZE）
 * 返回：分配的 skb 指针，或 NULL
 *
 * 参考：net/core/skbuff.c __alloc_skb()
 */
struct sk_buff *alloc_skb(unsigned int size);

/*
 * kfree_skb - 释放 sk_buff
 *
 * 参考：net/core/skbuff.c kfree_skb()
 */
void kfree_skb(struct sk_buff *skb);

/*
 * skb_reserve - 在 head 和 data 之间预留空间（headroom）
 *
 * 必须在 skb_put 之前调用。
 * 发送时预留空间给后续 skb_push 添加的协议头。
 *
 * @skb: sk_buff
 * @len: 预留字节数
 *
 * 参考：include/linux/skbuff.h skb_reserve()
 */
static inline void skb_reserve(struct sk_buff *skb, unsigned int len)
{
    skb->data += len;
    skb->tail += len;
}

/*
 * skb_put - 向 tail 方向追加数据空间
 *
 * 返回追加区域的起始地址（原 tail 位置）。
 * 调用者向返回的地址写入数据。
 *
 * @skb: sk_buff
 * @len: 追加字节数
 *
 * 参考：include/linux/skbuff.h skb_put()
 */
static inline void *skb_put(struct sk_buff *skb, unsigned int len)
{
    unsigned char *tmp = skb->tail;
    skb->tail += len;
    skb->len  += len;
    return tmp;
}

/*
 * skb_push - 向 data 方向添加头部空间
 *
 * data 指针前移，腾出空间给协议头。
 * 返回新的 data 位置。
 *
 * @skb: sk_buff
 * @len: 头部字节数
 *
 * 参考：include/linux/skbuff.h skb_push()
 */
static inline void *skb_push(struct sk_buff *skb, unsigned int len)
{
    skb->data -= len;
    skb->len  += len;
    return skb->data;
}

/*
 * skb_pull - 去除头部（data 后移）
 *
 * 接收路径中逐层剥离协议头。
 * 返回移动后的 data 位置。
 *
 * @skb: sk_buff
 * @len: 去除的字节数
 *
 * 参考：include/linux/skbuff.h skb_pull()
 */
static inline void *skb_pull(struct sk_buff *skb, unsigned int len)
{
    skb->len  -= len;
    return skb->data += len;
}

/*
 * skb_headroom - 获取 headroom 大小（head 到 data 的距离）
 */
static inline unsigned int skb_headroom(const struct sk_buff *skb)
{
    return (unsigned int)(skb->data - skb->head);
}

/*
 * skb_tailroom - 获取 tailroom 大小（tail 到 end 的距离）
 */
static inline unsigned int skb_tailroom(const struct sk_buff *skb)
{
    return (unsigned int)(skb->end - skb->tail);
}

/*
 * skb_copy_to - 将 skb 有效数据复制到目标缓冲区
 */
static inline void skb_copy_to(const struct sk_buff *skb, void *dst, unsigned int len)
{
    unsigned int copy_len = (len < skb->len) ? len : skb->len;
    unsigned int i;
    for (i = 0; i < copy_len; i++)
        ((unsigned char *)dst)[i] = skb->data[i];
}

/* skb 池初始化 */
void skb_init(void);

#endif /* __LINUX_SKBUFF_H */
