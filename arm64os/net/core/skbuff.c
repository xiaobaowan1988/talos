/* SPDX-License-Identifier: GPL-2.0 */
/*
 * arm64os/net/core/skbuff.c
 *
 * sk_buff（网络数据包缓冲区）分配与管理
 *
 * 参考：net/core/skbuff.c
 *
 * Phase 11 实现：
 *   - 静态 skb 池（MAX_SKBS 个）
 *   - alloc_skb()：从池中分配 skb，初始化 head/data/tail/end 指针
 *   - kfree_skb()：释放 skb 回池
 *
 * 简化说明：
 *   - 无 slab 分配器，使用静态数组
 *   - 无 skb_clone / skb_share
 *   - 无 skb_shinfo（无分散/聚集 I/O）
 */

#include <linux/types.h>
#include <linux/skbuff.h>
#include <linux/net.h>

/* 外部函数声明 */
void boot_printk(const char *s);
void boot_printk_hex(unsigned long val);

/* ---- skb 静态池 ---- */
static struct sk_buff skb_pool[MAX_SKBS];

/*
 * skb_init - 初始化 skb 池
 *
 * 将所有 skb 标记为未使用。
 */
void skb_init(void)
{
    int i;
    for (i = 0; i < MAX_SKBS; i++) {
        skb_pool[i].used = 0;
    }
    boot_printk("[net] skbuff pool initialized\n");
}

/*
 * alloc_skb - 分配一个 sk_buff
 *
 * @size: 请求的数据空间大小（必须 <= SKB_DATA_SIZE）
 *
 * 从静态池中找到一个空闲 skb，初始化指针：
 *   head = data = tail = buf（缓冲区起始）
 *   end = buf + SKB_DATA_SIZE（缓冲区结束）
 *
 * 调用者通常会先 skb_reserve() 预留 headroom，
 * 再 skb_put() 追加数据或 skb_push() 添加头部。
 *
 * 返回：分配的 skb 指针，或 NULL（池已满）
 *
 * 参考：net/core/skbuff.c __alloc_skb()
 */
struct sk_buff *alloc_skb(unsigned int size)
{
    int i;

    if (size > SKB_DATA_SIZE)
        return NULL;

    for (i = 0; i < MAX_SKBS; i++) {
        if (!skb_pool[i].used) {
            struct sk_buff *skb = &skb_pool[i];

            /* 标记为已使用 */
            skb->used = 1;

            /* 初始化指针 */
            skb->head = skb->buf;
            skb->data = skb->buf;
            skb->tail = skb->buf;
            skb->end  = skb->buf + SKB_DATA_SIZE;

            /* 清零元数据 */
            skb->sk       = NULL;
            skb->protocol = 0;
            skb->len      = 0;
            skb->nh       = NULL;
            skb->th       = NULL;

            return skb;
        }
    }

    boot_printk("[net] WARN: skb pool exhausted\n");
    return NULL;
}

/*
 * kfree_skb - 释放 sk_buff
 *
 * 将 skb 归还到静态池。
 *
 * 参考：net/core/skbuff.c kfree_skb()
 */
void kfree_skb(struct sk_buff *skb)
{
    if (!skb)
        return;
    skb->used = 0;
}
