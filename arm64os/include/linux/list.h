/* SPDX-License-Identifier: GPL-2.0 */
/*
 * arm64os/include/linux/list.h
 *
 * 内核双向循环链表
 *
 * 参考：include/linux/list.h
 *
 * Buddy分配器使用此链表管理每个 order 的空闲页块。
 * 与 Linux 内核实现保持接口一致。
 */

#ifndef __LINUX_LIST_H
#define __LINUX_LIST_H

/*
 * struct list_head - 双向循环链表节点
 *
 * 使用方式（侵入式链表，嵌入到宿主结构体中）：
 *   struct page {
 *       struct list_head lru;
 *       ...
 *   };
 *
 * 链表操作通过 container_of 宏从 list_head 指针找回宿主结构体。
 */
struct list_head {
    struct list_head *next;
    struct list_head *prev;
};

/*
 * INIT_LIST_HEAD - 初始化链表头节点（空链表：next/prev 均指向自身）
 */
static inline void INIT_LIST_HEAD(struct list_head *list)
{
    list->next = list;
    list->prev = list;
}

/*
 * list_empty - 判断链表是否为空
 */
static inline int list_empty(const struct list_head *head)
{
    return head->next == head;
}

/*
 * __list_add - 在 prev 和 next 之间插入 new 节点（内部辅助）
 */
static inline void __list_add(struct list_head *new,
                               struct list_head *prev,
                               struct list_head *next)
{
    next->prev = new;
    new->next  = next;
    new->prev  = prev;
    prev->next = new;
}

/*
 * list_add - 在链表头部插入（头插法，LIFO语义）
 *
 * 对 Buddy 分配器友好：最近释放的页块优先被重用（热缓存）。
 */
static inline void list_add(struct list_head *new, struct list_head *head)
{
    __list_add(new, head, head->next);
}

/*
 * list_add_tail - 在链表尾部插入（尾插法，FIFO语义）
 */
static inline void list_add_tail(struct list_head *new, struct list_head *head)
{
    __list_add(new, head->prev, head);
}

/*
 * __list_del - 从链表中摘除节点（内部辅助，不清除 next/prev）
 */
static inline void __list_del(struct list_head *prev, struct list_head *next)
{
    next->prev = prev;
    prev->next = next;
}

/*
 * list_del - 从链表中摘除节点，并使节点的 next/prev 指向无效位置
 *
 * 摘除后访问 next/prev 会导致明显错误（便于调试）。
 */
static inline void list_del(struct list_head *entry)
{
    __list_del(entry->prev, entry->next);
    entry->next = (struct list_head *)0xDEAD000000000000UL;
    entry->prev = (struct list_head *)0xDEAD000000000100UL;
}

/*
 * container_of - 通过成员指针找到宿主结构体
 *
 * 参数：
 *   ptr:    指向成员的指针
 *   type:   宿主结构体类型
 *   member: 成员名
 *
 * 实现：将 ptr 减去 member 在 type 中的偏移量。
 */
#define container_of(ptr, type, member) \
    ((type *)((char *)(ptr) - __builtin_offsetof(type, member)))

/*
 * list_entry - 通过 list_head 指针获取宿主结构体
 */
#define list_entry(ptr, type, member) \
    container_of(ptr, type, member)

/*
 * list_first_entry - 获取链表第一个元素（链表必须非空）
 */
#define list_first_entry(head, type, member) \
    list_entry((head)->next, type, member)

/*
 * list_for_each - 遍历链表（只读，不可在循环体内删除当前节点）
 */
#define list_for_each(pos, head) \
    for (pos = (head)->next; pos != (head); pos = pos->next)

/*
 * list_for_each_safe - 安全遍历链表（可在循环体内删除当前节点）
 */
#define list_for_each_safe(pos, n, head) \
    for (pos = (head)->next, n = pos->next; \
         pos != (head); \
         pos = n, n = pos->next)

#endif /* __LINUX_LIST_H */
