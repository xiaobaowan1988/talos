/* SPDX-License-Identifier: GPL-2.0 */
/*
 * arm64os/net/netfilter/core.c
 *
 * netfilter 钩子框架 — 注册、注销、执行
 *
 * 参考：net/netfilter/core.c
 *
 * Phase 11 实现：
 *   - nf_init()：初始化所有 Hook 点
 *   - nf_register_net_hook()：注册钩子（按优先级排序插入）
 *   - nf_unregister_net_hook()：注销钩子
 *   - nf_hook_slow()：遍历执行某 Hook 点的所有钩子
 *   - nf_hook()：便捷接口
 *
 * 简化说明：
 *   - 全局（init_net）共享 Hook 数组
 *   - 每个 Hook 点最多 NF_MAX_HOOKS_PER_POINT 个钩子
 *   - 按优先级升序排列（priority 越小越先执行）
 *   - 无 RCU / 无并发保护
 */

#include <linux/types.h>
#include <linux/netfilter.h>
#include <linux/skbuff.h>
#include <linux/nsproxy.h>

/* 外部函数声明 */
void boot_printk(const char *s);
void boot_printk_hex(unsigned long val);

/* ---- 全局 Hook 数据 ---- */

/*
 * 每个 Hook 点维护一个 nf_hook_entries 结构。
 * init_net.nf.hooks_ipv4[hooknum] 指向对应的 entries。
 *
 * 简化：直接使用全局数组（单 network namespace）。
 */
static struct nf_hook_entries nf_entries[NF_INET_NUMHOOKS];

/*
 * nf_init - 初始化 netfilter 框架
 *
 * 参考：net/netfilter/core.c nf_init()
 */
void nf_init(void)
{
    int i, j;

    for (i = 0; i < NF_INET_NUMHOOKS; i++) {
        nf_entries[i].num_hook_entries = 0;
        for (j = 0; j < NF_MAX_HOOKS_PER_POINT; j++) {
            nf_entries[i].hooks[j].hook = NULL;
            nf_entries[i].hooks[j].priv = NULL;
            nf_entries[i].hooks[j].priority = 0;
        }
    }

    /* 将 entries 注册到 init_net */
    for (i = 0; i < NF_INET_NUMHOOKS; i++)
        init_net.nf_hooks_ipv4[i] = &nf_entries[i];

    boot_printk("[net] netfilter framework initialized\n");
}

/*
 * nf_register_net_hook - 注册一个 netfilter 钩子
 *
 * 参考：net/netfilter/core.c nf_register_net_hook()
 *
 * 将 hook 函数按优先级（升序）插入到对应 Hook 点的 entries 数组。
 * 优先级越小的钩子越先执行。
 *
 * @net: 网络命名空间（本实现仅使用 init_net）
 * @ops: 钩子操作结构
 *
 * 返回：0 成功，-1 失败
 */
int nf_register_net_hook(struct net *net, const struct nf_hook_ops *ops)
{
    struct nf_hook_entries *entries;
    int hooknum;
    int n, i, insert_pos;

    if (!ops || !ops->hook)
        return -1;

    hooknum = (int)ops->hooknum;
    if (hooknum < 0 || hooknum >= NF_INET_NUMHOOKS)
        return -1;

    entries = &nf_entries[hooknum];
    n = entries->num_hook_entries;

    if (n >= NF_MAX_HOOKS_PER_POINT) {
        boot_printk("[nf] WARN: hook point full\n");
        return -1;
    }

    /* 找到按优先级的插入位置 */
    insert_pos = n;
    for (i = 0; i < n; i++) {
        if (ops->priority < entries->hooks[i].priority) {
            insert_pos = i;
            break;
        }
    }

    /* 后移已有条目，腾出插入位置 */
    for (i = n; i > insert_pos; i--) {
        entries->hooks[i] = entries->hooks[i - 1];
    }

    /* 插入新钩子 */
    entries->hooks[insert_pos].hook = ops->hook;
    entries->hooks[insert_pos].priv = ops->priv;
    entries->hooks[insert_pos].priority = ops->priority;
    entries->num_hook_entries = n + 1;

    return 0;
}

/*
 * nf_unregister_net_hook - 注销一个 netfilter 钩子
 *
 * 参考：net/netfilter/core.c nf_unregister_net_hook()
 *
 * 从对应 Hook 点找到并移除匹配的钩子。
 */
void nf_unregister_net_hook(struct net *net, const struct nf_hook_ops *ops)
{
    struct nf_hook_entries *entries;
    int hooknum;
    int n, i, found;

    if (!ops)
        return;

    hooknum = (int)ops->hooknum;
    if (hooknum < 0 || hooknum >= NF_INET_NUMHOOKS)
        return;

    entries = &nf_entries[hooknum];
    n = entries->num_hook_entries;

    found = -1;
    for (i = 0; i < n; i++) {
        if (entries->hooks[i].hook == ops->hook &&
            entries->hooks[i].priv == ops->priv) {
            found = i;
            break;
        }
    }

    if (found < 0)
        return;

    /* 前移后续条目 */
    for (i = found; i < n - 1; i++) {
        entries->hooks[i] = entries->hooks[i + 1];
    }
    entries->hooks[n - 1].hook = NULL;
    entries->hooks[n - 1].priv = NULL;
    entries->num_hook_entries = n - 1;
}

/*
 * nf_hook_slow - 遍历并执行某 Hook 点的所有钩子
 *
 * 参考：net/netfilter/core.c nf_hook_slow()
 *
 * 顺序执行 entries 中的每个钩子。
 * 如果某个钩子返回 NF_DROP，立即停止并返回 NF_DROP。
 * 如果某个钩子返回 NF_STOLEN，停止但不丢弃（钩子接管了包）。
 * 所有钩子都返回 NF_ACCEPT 才最终返回 NF_ACCEPT。
 */
unsigned int nf_hook_slow(struct sk_buff *skb,
                          struct nf_hook_state *state,
                          const struct nf_hook_entries *e)
{
    unsigned int verdict = NF_ACCEPT;
    int i;

    if (!e || !skb)
        return NF_ACCEPT;

    for (i = 0; i < e->num_hook_entries; i++) {
        if (!e->hooks[i].hook)
            continue;

        verdict = e->hooks[i].hook(e->hooks[i].priv, skb, state);
        if (verdict != NF_ACCEPT)
            break;
    }

    return verdict;
}

/*
 * nf_hook - 便捷接口：执行某 Hook 点
 *
 * 参考：include/linux/netfilter.h NF_HOOK()
 *
 * @net:     网络命名空间
 * @hooknum: Hook 点编号（NF_INET_*）
 * @skb:     待处理的数据包
 *
 * 返回：NF_ACCEPT / NF_DROP / NF_STOLEN
 */
unsigned int nf_hook(struct net *net, unsigned int hooknum,
                     struct sk_buff *skb)
{
    struct nf_hook_entries *entries;
    struct nf_hook_state state;

    if (hooknum >= NF_INET_NUMHOOKS)
        return NF_ACCEPT;

    entries = net->nf_hooks_ipv4[hooknum];
    if (!entries || entries->num_hook_entries == 0)
        return NF_ACCEPT;   /* 无钩子，默认接受 */

    /* 构建 Hook 执行上下文 */
    state.hook = hooknum;
    state.pf   = NFPROTO_IPV4;
    state.net  = net;

    return nf_hook_slow(skb, &state, entries);
}
