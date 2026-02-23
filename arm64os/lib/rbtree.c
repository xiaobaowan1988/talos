/* SPDX-License-Identifier: GPL-2.0 */
/*
 * arm64os/lib/rbtree.c
 *
 * 红黑树实现（简化版）
 *
 * 参考：lib/rbtree.c
 *
 * 红黑树性质：
 *   1. 每个节点是红色或黑色
 *   2. 根节点是黑色
 *   3. 叶子节点（NULL）是黑色
 *   4. 红色节点的两个子节点都是黑色（不允许连续红色）
 *   5. 从任一节点到其所有叶子的路径上黑色节点数相同
 *
 * CFS 调度器依赖红黑树的 O(log N) 插入/删除和 O(1) 最小值查找。
 */

#include <linux/rbtree.h>

/*
 * ============================================================
 * 辅助函数：旋转
 * ============================================================
 */

/*
 * __rb_rotate_left - 左旋
 *
 *     P               P
 *     |               |
 *     X       →       Y
 *    / \             / \
 *   a   Y           X   c
 *      / \         / \
 *     b   c       a   b
 */
static void __rb_rotate_left(struct rb_node *node, struct rb_root *root)
{
    struct rb_node *right = node->rb_right;
    struct rb_node *parent = rb_parent(node);

    node->rb_right = right->rb_left;
    if (right->rb_left)
        rb_set_parent(right->rb_left, node);

    right->rb_left = node;
    rb_set_parent(right, parent);

    if (parent) {
        if (node == parent->rb_left)
            parent->rb_left = right;
        else
            parent->rb_right = right;
    } else {
        root->rb_node = right;
    }
    rb_set_parent(node, right);
}

/*
 * __rb_rotate_right - 右旋
 *
 *       P             P
 *       |             |
 *       X     →       Y
 *      / \           / \
 *     Y   c         a   X
 *    / \               / \
 *   a   b             b   c
 */
static void __rb_rotate_right(struct rb_node *node, struct rb_root *root)
{
    struct rb_node *left = node->rb_left;
    struct rb_node *parent = rb_parent(node);

    node->rb_left = left->rb_right;
    if (left->rb_right)
        rb_set_parent(left->rb_right, node);

    left->rb_right = node;
    rb_set_parent(left, parent);

    if (parent) {
        if (node == parent->rb_right)
            parent->rb_right = left;
        else
            parent->rb_left = left;
    } else {
        root->rb_node = left;
    }
    rb_set_parent(node, left);
}

/*
 * ============================================================
 * rb_insert_color - 插入后的修复
 *
 * 新节点总是作为红色插入。如果其父节点也是红色，
 * 则违反了性质4，需要通过旋转和变色修复。
 *
 * 参考：lib/rbtree.c __rb_insert()
 * ============================================================
 */
void rb_insert_color(struct rb_node *node, struct rb_root *root)
{
    struct rb_node *parent, *gparent, *uncle;

    while ((parent = rb_parent(node)) && rb_is_red(parent)) {
        gparent = rb_parent(parent);

        if (parent == gparent->rb_left) {
            uncle = gparent->rb_right;

            /* Case 1: 叔叔是红色 → 变色，向上递归 */
            if (uncle && rb_is_red(uncle)) {
                rb_set_black(parent);
                rb_set_black(uncle);
                rb_set_red(gparent);
                node = gparent;
                continue;
            }

            /* Case 2: 叔叔是黑色，node 是右子 → 左旋转为 Case 3 */
            if (node == parent->rb_right) {
                __rb_rotate_left(parent, root);
                /* 旋转后 parent 和 node 互换 */
                node = parent;
                parent = rb_parent(node);
            }

            /* Case 3: 叔叔是黑色，node 是左子 → 右旋 + 变色 */
            rb_set_black(parent);
            rb_set_red(gparent);
            __rb_rotate_right(gparent, root);
        } else {
            /* 对称情况：parent 是 gparent 的右子 */
            uncle = gparent->rb_left;

            if (uncle && rb_is_red(uncle)) {
                rb_set_black(parent);
                rb_set_black(uncle);
                rb_set_red(gparent);
                node = gparent;
                continue;
            }

            if (node == parent->rb_left) {
                __rb_rotate_right(parent, root);
                node = parent;
                parent = rb_parent(node);
            }

            rb_set_black(parent);
            rb_set_red(gparent);
            __rb_rotate_left(gparent, root);
        }
    }

    rb_set_black(root->rb_node);
}

/*
 * ============================================================
 * __rb_erase_color - 删除后的修复（处理双黑情况）
 *
 * 当删除的节点（或其替代者）是黑色时，从该位置到根的路径
 * 少了一个黑色节点，需要修复以恢复性质5。
 *
 * 参考：lib/rbtree.c ____rb_erase_color()
 * ============================================================
 */
static void __rb_erase_color(struct rb_node *node, struct rb_node *parent,
                             struct rb_root *root)
{
    struct rb_node *sibling;

    while ((!node || rb_is_black(node)) && node != root->rb_node) {
        if (node == parent->rb_left) {
            sibling = parent->rb_right;

            /* Case 1: 兄弟是红色 → 旋转 + 变色，转为 Case 2/3/4 */
            if (rb_is_red(sibling)) {
                rb_set_black(sibling);
                rb_set_red(parent);
                __rb_rotate_left(parent, root);
                sibling = parent->rb_right;
            }

            /* Case 2: 兄弟的两个子节点都是黑色 → 变色，向上递归 */
            if ((!sibling->rb_left || rb_is_black(sibling->rb_left)) &&
                (!sibling->rb_right || rb_is_black(sibling->rb_right))) {
                rb_set_red(sibling);
                node = parent;
                parent = rb_parent(node);
            } else {
                /* Case 3: 兄弟的右子是黑色 → 右旋兄弟，转为 Case 4 */
                if (!sibling->rb_right || rb_is_black(sibling->rb_right)) {
                    if (sibling->rb_left)
                        rb_set_black(sibling->rb_left);
                    rb_set_red(sibling);
                    __rb_rotate_right(sibling, root);
                    sibling = parent->rb_right;
                }

                /* Case 4: 兄弟的右子是红色 → 左旋 + 变色 */
                rb_set_parent_color(sibling, rb_parent(parent),
                                    rb_color(parent));
                rb_set_black(parent);
                if (sibling->rb_right)
                    rb_set_black(sibling->rb_right);
                __rb_rotate_left(parent, root);
                node = root->rb_node;
                break;
            }
        } else {
            /* 对称情况 */
            sibling = parent->rb_left;

            if (rb_is_red(sibling)) {
                rb_set_black(sibling);
                rb_set_red(parent);
                __rb_rotate_right(parent, root);
                sibling = parent->rb_left;
            }

            if ((!sibling->rb_right || rb_is_black(sibling->rb_right)) &&
                (!sibling->rb_left || rb_is_black(sibling->rb_left))) {
                rb_set_red(sibling);
                node = parent;
                parent = rb_parent(node);
            } else {
                if (!sibling->rb_left || rb_is_black(sibling->rb_left)) {
                    if (sibling->rb_right)
                        rb_set_black(sibling->rb_right);
                    rb_set_red(sibling);
                    __rb_rotate_left(sibling, root);
                    sibling = parent->rb_left;
                }

                rb_set_parent_color(sibling, rb_parent(parent),
                                    rb_color(parent));
                rb_set_black(parent);
                if (sibling->rb_left)
                    rb_set_black(sibling->rb_left);
                __rb_rotate_right(parent, root);
                node = root->rb_node;
                break;
            }
        }
    }

    if (node)
        rb_set_black(node);
}

/*
 * ============================================================
 * rb_erase - 从红黑树中删除节点
 *
 * 标准 BST 删除 + 红黑树修复。
 * 参考：lib/rbtree.c __rb_erase_augmented()
 * ============================================================
 */
void rb_erase(struct rb_node *node, struct rb_root *root)
{
    struct rb_node *child, *parent;
    int color;

    if (!node->rb_left) {
        child = node->rb_right;
    } else if (!node->rb_right) {
        child = node->rb_left;
    } else {
        /* 两个子节点都存在：找中序后继（右子树的最左节点）*/
        struct rb_node *old = node;
        struct rb_node *left;

        node = node->rb_right;
        while ((left = node->rb_left) != NULL)
            node = left;

        /* node 是中序后继，替换 old 的位置 */
        child = node->rb_right;
        parent = rb_parent(node);
        color = rb_color(node);

        if (child)
            rb_set_parent(child, parent);

        if (parent == old) {
            parent->rb_right = child;
            parent = node;
        } else {
            parent->rb_left = child;
        }

        node->__rb_parent_color = old->__rb_parent_color;
        node->rb_right = old->rb_right;
        node->rb_left = old->rb_left;

        if (rb_parent(old)) {
            if (rb_parent(old)->rb_left == old)
                rb_parent(old)->rb_left = node;
            else
                rb_parent(old)->rb_right = node;
        } else {
            root->rb_node = node;
        }

        rb_set_parent(old->rb_left, node);
        if (old->rb_right)
            rb_set_parent(old->rb_right, node);

        if (color == RB_BLACK)
            __rb_erase_color(child, parent, root);
        return;
    }

    /* 节点最多一个子节点 */
    parent = rb_parent(node);
    color = rb_color(node);

    if (child)
        rb_set_parent(child, parent);

    if (parent) {
        if (parent->rb_left == node)
            parent->rb_left = child;
        else
            parent->rb_right = child;
    } else {
        root->rb_node = child;
    }

    if (color == RB_BLACK)
        __rb_erase_color(child, parent, root);
}

/*
 * ============================================================
 * 带缓存（最左节点）的操作
 * ============================================================
 */

void rb_insert_color_cached(struct rb_node *node,
                            struct rb_root_cached *root,
                            bool leftmost)
{
    if (leftmost)
        root->rb_leftmost = node;
    rb_insert_color(node, &root->rb_root);
}

void rb_erase_cached(struct rb_node *node, struct rb_root_cached *root)
{
    if (root->rb_leftmost == node)
        root->rb_leftmost = rb_next(node);
    rb_erase(node, &root->rb_root);
}

/*
 * ============================================================
 * 遍历操作
 * ============================================================
 */

/*
 * rb_first - 返回树中最左节点（中序第一个 = 最小值）
 */
struct rb_node *rb_first(const struct rb_root *root)
{
    struct rb_node *n = root->rb_node;
    if (!n)
        return NULL;
    while (n->rb_left)
        n = n->rb_left;
    return n;
}

/*
 * rb_next - 中序遍历的下一个节点
 *
 * 如果有右子树，返回右子树的最左节点。
 * 否则，向上回溯到第一个"从左子来"的祖先。
 */
struct rb_node *rb_next(const struct rb_node *node)
{
    struct rb_node *parent;

    if (!node)
        return NULL;

    /* 有右子树：返回右子树最左节点 */
    if (node->rb_right) {
        struct rb_node *n = node->rb_right;
        while (n->rb_left)
            n = n->rb_left;
        return n;
    }

    /* 无右子树：向上找到第一个从左子来的祖先 */
    while ((parent = rb_parent(node)) && node == parent->rb_right)
        node = parent;

    return parent;
}
