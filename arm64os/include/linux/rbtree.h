/* SPDX-License-Identifier: GPL-2.0 */
/*
 * arm64os/include/linux/rbtree.h
 *
 * 红黑树接口（简化版）
 *
 * 参考：include/linux/rbtree.h, include/linux/rbtree_augmented.h
 *
 * 红黑树用于 CFS 调度器的 tasks_timeline，按 vruntime 排序。
 * rb_root_cached 额外缓存最左（最小 vruntime）节点，
 * 使 pick_next_entity() 为 O(1)。
 *
 * 实现在 lib/rbtree.c 中。
 */

#ifndef __LINUX_RBTREE_H
#define __LINUX_RBTREE_H

#include <linux/types.h>

/*
 * struct rb_node - 红黑树节点
 *
 * __rb_parent_color 将父节点指针和颜色位合并存储：
 *   - bit 0 = color（0=RED, 1=BLACK）
 *   - bits [63:1] = parent pointer（最低位对齐，天然为0）
 *
 * 参考：lib/rbtree.c
 */
struct rb_node {
    unsigned long  __rb_parent_color;
    struct rb_node *rb_right;
    struct rb_node *rb_left;
};

struct rb_root {
    struct rb_node *rb_node;
};

/*
 * struct rb_root_cached - 带最左节点缓存的红黑树根
 *
 * CFS 调度器总是需要取 vruntime 最小的节点（最左节点），
 * 缓存后 pick_next_entity() 为 O(1) 而非 O(log N)。
 */
struct rb_root_cached {
    struct rb_root  rb_root;
    struct rb_node *rb_leftmost;
};

/* 颜色常量 */
#define RB_RED      0
#define RB_BLACK    1

/* 初始化宏 */
#define RB_ROOT         (struct rb_root) { NULL }
#define RB_ROOT_CACHED  (struct rb_root_cached) { { NULL }, NULL }

/* 从 __rb_parent_color 提取父节点指针 */
#define rb_parent(r)    ((struct rb_node *)((r)->__rb_parent_color & ~3UL))

/* 从 __rb_parent_color 提取颜色 */
#define rb_color(r)     ((r)->__rb_parent_color & 1UL)

/* 判断节点是否为红/黑 */
#define rb_is_red(r)    (!rb_color(r))
#define rb_is_black(r)  (rb_color(r))

/* 设置父节点（保留颜色）*/
static inline void rb_set_parent(struct rb_node *rb, struct rb_node *p)
{
    rb->__rb_parent_color = (rb->__rb_parent_color & 1UL) | (unsigned long)p;
}

/* 设置颜色（保留父节点）*/
static inline void rb_set_parent_color(struct rb_node *rb,
                                       struct rb_node *p,
                                       int color)
{
    rb->__rb_parent_color = (unsigned long)p | (unsigned long)color;
}

static inline void rb_set_black(struct rb_node *rb)
{
    rb->__rb_parent_color |= RB_BLACK;
}

static inline void rb_set_red(struct rb_node *rb)
{
    rb->__rb_parent_color &= ~1UL;
}

/*
 * rb_entry - 从 rb_node 指针获取宿主结构体
 */
#define rb_entry(ptr, type, member) \
    container_of(ptr, type, member)

/*
 * rb_link_node - 将节点链接到树中（颜色为红色，无子节点）
 *
 * @node:   要插入的节点
 * @parent: 父节点（由调用者遍历树时确定）
 * @rb_link: 父节点的 rb_left 或 rb_right 指针的地址
 *
 * 调用后还需调用 rb_insert_color() 做旋转/变色。
 */
static inline void rb_link_node(struct rb_node *node,
                                struct rb_node *parent,
                                struct rb_node **rb_link)
{
    node->__rb_parent_color = (unsigned long)parent; /* RED = 0 */
    node->rb_left = NULL;
    node->rb_right = NULL;
    *rb_link = node;
}

/*
 * 红黑树核心操作（实现在 lib/rbtree.c）
 */
void rb_insert_color(struct rb_node *node, struct rb_root *root);
void rb_erase(struct rb_node *node, struct rb_root *root);

/* 带缓存的插入/删除 */
void rb_insert_color_cached(struct rb_node *node,
                            struct rb_root_cached *root,
                            bool leftmost);
void rb_erase_cached(struct rb_node *node, struct rb_root_cached *root);

/*
 * rb_first - 返回树中最左（最小）节点
 */
struct rb_node *rb_first(const struct rb_root *root);

/*
 * rb_next - 返回中序遍历的下一个节点
 */
struct rb_node *rb_next(const struct rb_node *node);

/*
 * rb_first_cached - 从缓存返回最左节点（O(1)）
 */
static inline struct rb_node *rb_first_cached(const struct rb_root_cached *root)
{
    return root->rb_leftmost;
}

/*
 * RB_EMPTY_ROOT - 判断树是否为空
 */
static inline int RB_EMPTY_ROOT(const struct rb_root *root)
{
    return root->rb_node == NULL;
}

#endif /* __LINUX_RBTREE_H */
