/* SPDX-License-Identifier: GPL-2.0 */
/*
 * arm64os/kernel/sched/fair.c
 *
 * CFS（完全公平调度器）实现
 *
 * 参考：kernel/sched/fair.c
 *
 * CFS 的核心思想：
 *   每个进程维护一个 vruntime（虚拟运行时间），CFS 始终选择 vruntime
 *   最小的进程运行。高优先级（大 weight）进程的 vruntime 增长慢，
 *   低优先级进程 vruntime 增长快，从而实现按权重比例分配 CPU 时间。
 *
 * 关键公式：
 *   vruntime += actual_runtime × (NICE_0_WEIGHT / task_weight)
 *
 * 数据结构：
 *   - 红黑树（rb_root_cached）按 vruntime 排序所有可运行进程
 *   - rb_leftmost 缓存最小 vruntime 节点，pick_next 为 O(1)
 *   - min_vruntime 单调递增，作为新进程入队的基准值
 */

#include <linux/types.h>
#include <linux/sched.h>
#include <linux/rbtree.h>
#include <linux/list.h>

/*
 * ============================================================
 * nice 值到权重的映射表
 *
 * 索引 = nice + 20（nice=-20 → 索引0，nice=19 → 索引39）
 * 每相邻 nice 值的权重比约为 1.25:1（~10% CPU 差异）
 *
 * 参考：kernel/sched/core.c sched_prio_to_weight[]
 * ============================================================
 */
const unsigned long sched_prio_to_weight[40] = {
 /* nice -20 */  88761, 71755, 56483, 46273, 36291,
 /* nice -15 */  29154, 23254, 18705, 14949, 11916,
 /* nice -10 */   9548,  7620,  6100,  4904,  3906,
 /* nice  -5 */   3121,  2501,  1991,  1586,  1277,
 /* nice   0 */   1024,   820,   655,   526,   423,
 /* nice   5 */    335,   272,   215,   172,   137,
 /* nice  10 */    110,    87,    70,    56,    45,
 /* nice  15 */     36,    29,    23,    18,    15,
};

/*
 * ============================================================
 * sched_clock - 调度器时钟（纳秒精度）
 *
 * 读取 ARMv8 Virtual Counter（CNTVCT_EL0）并转换为纳秒。
 * QEMU virt 的 CNTFRQ = 62.5 MHz。
 *
 * 参考：kernel/sched/clock.c sched_clock_cpu()
 * ============================================================
 */
uint64_t sched_clock(void)
{
    uint64_t cnt, freq;
    __asm__ volatile("mrs %0, cntvct_el0" : "=r"(cnt));
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(freq));
    /* cnt * 1e9 / freq，避免溢出：先除再乘（精度损失可接受）*/
    return (cnt / freq) * 1000000000ULL +
           ((cnt % freq) * 1000000000ULL) / freq;
}

/*
 * ============================================================
 * update_min_vruntime - 更新 cfs_rq 的 min_vruntime
 *
 * min_vruntime 是单调递增的，作为新进程入队时的 vruntime 基准。
 * 它取 curr->vruntime 和 rb_leftmost->vruntime 的最小值，
 * 但不会低于当前 min_vruntime（保证单调性）。
 *
 * 参考：kernel/sched/fair.c update_min_vruntime()
 * ============================================================
 */
void update_min_vruntime(struct cfs_rq *cfs_rq)
{
    uint64_t vruntime = cfs_rq->min_vruntime;
    struct sched_entity *curr = cfs_rq->curr;
    struct rb_node *leftmost = rb_first_cached(&cfs_rq->tasks_timeline);

    if (curr) {
        if (curr->on_rq)
            vruntime = curr->vruntime;
        else if (leftmost) {
            struct sched_entity *se;
            se = rb_entry(leftmost, struct sched_entity, run_node);
            vruntime = se->vruntime;
        }
    }

    if (leftmost) {
        struct sched_entity *se;
        se = rb_entry(leftmost, struct sched_entity, run_node);
        if (!curr || !curr->on_rq)
            vruntime = se->vruntime;
        else if (se->vruntime < vruntime)
            vruntime = se->vruntime;
    }

    /* min_vruntime 只能递增 */
    if ((s64)(vruntime - cfs_rq->min_vruntime) > 0)
        cfs_rq->min_vruntime = vruntime;
}

/*
 * ============================================================
 * update_curr - 更新当前进程的 vruntime
 *
 * 在每次调度决策前调用（scheduler_tick、schedule 等）。
 *
 * 参考：kernel/sched/fair.c update_curr()
 * ============================================================
 */
void update_curr(struct cfs_rq *cfs_rq)
{
    struct sched_entity *curr = cfs_rq->curr;
    uint64_t now, delta_exec;

    if (!curr)
        return;

    now = sched_clock();

    if (now <= curr->exec_start)
        return;

    delta_exec = now - curr->exec_start;
    curr->exec_start = now;
    curr->sum_exec_runtime += delta_exec;

    /*
     * vruntime += delta_exec × (NICE_0_WEIGHT / task_weight)
     *
     * 高权重进程的 vruntime 增长慢（获得更多 CPU 时间），
     * 低权重进程的 vruntime 增长快（获得较少 CPU 时间）。
     */
    if (curr->load.weight > 0)
        curr->vruntime += (delta_exec * NICE_0_WEIGHT) / curr->load.weight;

    update_min_vruntime(cfs_rq);
}

/*
 * ============================================================
 * __enqueue_entity - 将调度实体插入红黑树
 *
 * 按 vruntime 排序，较小的在左边。
 * leftmost 标记是否为新的最左节点（用于 rb_root_cached 缓存）。
 *
 * 参考：kernel/sched/fair.c __enqueue_entity()
 * ============================================================
 */
static void __enqueue_entity(struct cfs_rq *cfs_rq, struct sched_entity *se)
{
    struct rb_node **link = &cfs_rq->tasks_timeline.rb_root.rb_node;
    struct rb_node *parent = NULL;
    struct sched_entity *entry;
    bool leftmost = true;

    while (*link) {
        parent = *link;
        entry = rb_entry(parent, struct sched_entity, run_node);

        /*
         * 使用有符号比较处理 vruntime 回绕：
         * (s64)(a - b) < 0 等价于 a < b（在 vruntime 空间中）
         */
        if ((s64)(se->vruntime - entry->vruntime) < 0) {
            link = &parent->rb_left;
        } else {
            link = &parent->rb_right;
            leftmost = false;
        }
    }

    rb_link_node(&se->run_node, parent, link);
    rb_insert_color_cached(&se->run_node, &cfs_rq->tasks_timeline, leftmost);
}

/*
 * ============================================================
 * __dequeue_entity - 从红黑树中移除调度实体
 *
 * 参考：kernel/sched/fair.c __dequeue_entity()
 * ============================================================
 */
static void __dequeue_entity(struct cfs_rq *cfs_rq, struct sched_entity *se)
{
    rb_erase_cached(&se->run_node, &cfs_rq->tasks_timeline);
}

/*
 * ============================================================
 * enqueue_entity - 将调度实体加入 CFS 运行队列
 *
 * 1. 将 vruntime 对齐到 min_vruntime（防止新进程饿死旧进程）
 * 2. 插入红黑树
 * 3. 更新运行队列统计
 *
 * 参考：kernel/sched/fair.c enqueue_entity()
 * ============================================================
 */
void enqueue_entity(struct cfs_rq *cfs_rq, struct sched_entity *se)
{
    /*
     * 新进程（vruntime=0）或长时间睡眠的进程，
     * 将 vruntime 设为 min_vruntime，避免它获得不公平的超长运行时间。
     */
    if ((s64)(se->vruntime - cfs_rq->min_vruntime) < 0)
        se->vruntime = cfs_rq->min_vruntime;

    if (se->on_rq)
        return;

    se->on_rq = 1;
    __enqueue_entity(cfs_rq, se);
    cfs_rq->nr_running++;
    cfs_rq->load.weight += se->load.weight;
}

/*
 * ============================================================
 * dequeue_entity - 从 CFS 运行队列移除调度实体
 *
 * 参考：kernel/sched/fair.c dequeue_entity()
 * ============================================================
 */
void dequeue_entity(struct cfs_rq *cfs_rq, struct sched_entity *se)
{
    if (!se->on_rq)
        return;

    __dequeue_entity(cfs_rq, se);
    se->on_rq = 0;
    cfs_rq->nr_running--;
    cfs_rq->load.weight -= se->load.weight;

    update_min_vruntime(cfs_rq);
}

/*
 * ============================================================
 * pick_next_entity - 选择 vruntime 最小的调度实体
 *
 * 利用 rb_root_cached 的最左节点缓存，O(1) 获取最小 vruntime。
 *
 * 参考：kernel/sched/fair.c pick_next_entity()
 * ============================================================
 */
struct sched_entity *pick_next_entity(struct cfs_rq *cfs_rq)
{
    struct rb_node *left = rb_first_cached(&cfs_rq->tasks_timeline);
    if (!left)
        return NULL;
    return rb_entry(left, struct sched_entity, run_node);
}
