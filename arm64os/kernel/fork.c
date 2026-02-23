/* SPDX-License-Identifier: GPL-2.0 */
/*
 * arm64os/kernel/fork.c
 *
 * 用户进程创建
 *
 * 参考：kernel/fork.c
 *       arch/arm64/kernel/process.c
 *
 * Phase 5 实现：
 *   - create_user_task()：创建用户态进程并加入调度队列。
 *     与 kernel_thread_create() 类似，但设置用户态入口：
 *     新进程首次被 schedule() 选中时，通过 ret_to_user 跳转到 EL0。
 *
 * Phase 5 简化：
 *   - 不支持 fork()（复制进程），仅支持从内核创建新用户进程
 *   - 不复制页表（每个用户进程有独立的 pgd）
 *   - 无信号、无文件描述符等
 */

#include <linux/types.h>
#include <linux/sched.h>
#include <asm/memory.h>

/* 外部函数 */
void boot_printk(const char *s);
void boot_printk_hex(unsigned long val);

/* Buddy 分配器 */
struct page;
struct page *alloc_pages(unsigned int order);
void *page_address(struct page *page);

/* 调度器 */
void enqueue_entity(struct cfs_rq *cfs_rq, struct sched_entity *se);
struct rq *this_rq(void);
extern const unsigned long sched_prio_to_weight[40];

/* 进程池（在 kernel/sched/core.c 中定义） */
extern struct task_struct *current_task;

/* entry.S 中的 ret_to_user */
extern void ret_to_user(void);

/*
 * pt_regs 结构体（与 entry.S 的 PT_* 偏移量严格对应）
 */
struct pt_regs {
    unsigned long regs[31];     /* x0-x30 */
    unsigned long sp;           /* 用户态 SP_EL0 */
    unsigned long pc;           /* ELR_EL1 */
    unsigned long pstate;       /* SPSR_EL1 */
};

/* pt_regs 大小 */
#define PT_REGS_SIZE    272     /* 34 × 8 */

/*
 * 静态进程池（与 core.c 共享）
 * Phase 5：使用 core.c 的 task_pool，通过 alloc_task_struct 获取。
 */
#define MAX_USER_TASKS  8

static struct task_struct user_task_pool[MAX_USER_TASKS];
static int next_user_pid = 100;  /* 用户进程 PID 从 100 开始 */

/* mm_struct 池 */
static struct mm_struct mm_pool[MAX_USER_TASKS];
static int mm_pool_idx = 0;

/*
 * ============================================================
 * create_user_task - 创建用户态进程
 *
 * @pgd_phys:  用户页表 PGD 物理地址
 * @entry_pc:  用户态入口 PC（ELF 的 e_entry）
 * @user_sp:   用户态栈指针（SP_EL0）
 * @name:      进程名
 *
 * 流程：
 *   1. 分配 task_struct
 *   2. 分配内核栈（8KB）
 *   3. 在内核栈顶构造 pt_regs（用户态初始寄存器）
 *   4. 设置 cpu_context：
 *      - sp = pt_regs 地址（栈上）
 *      - pc = ret_to_user（首次调度时跳转到此，执行 kernel_exit 0）
 *   5. 设置 mm_struct 指向用户页表
 *   6. 加入 CFS 调度队列
 *
 * 返回：task_struct 指针，NULL 表示失败。
 *
 * 参考：kernel/fork.c copy_process + copy_thread
 * ============================================================
 */
struct task_struct *create_user_task(unsigned long pgd_phys,
                                     unsigned long entry_pc,
                                     unsigned long user_sp,
                                     const char *name)
{
    struct task_struct *tsk;
    struct mm_struct *mm;
    struct page *stack_page;
    unsigned long stack_base, stack_top;
    struct pt_regs *uregs;
    struct rq *rq;
    int i;

    /* 分配 task_struct */
    if (next_user_pid - 100 >= MAX_USER_TASKS) {
        boot_printk("[fork] ERROR: user task pool exhausted\n");
        return NULL;
    }
    tsk = &user_task_pool[next_user_pid - 100];

    /* 分配 mm_struct */
    if (mm_pool_idx >= MAX_USER_TASKS) {
        boot_printk("[fork] ERROR: mm pool exhausted\n");
        return NULL;
    }
    mm = &mm_pool[mm_pool_idx++];
    mm->pgd = pgd_phys;

    /* 分配内核栈（2 页 = 8KB） */
    stack_page = alloc_pages(1);
    if (!stack_page) {
        boot_printk("[fork] ERROR: cannot allocate kernel stack\n");
        return NULL;
    }
    stack_base = (unsigned long)page_address(stack_page);
    stack_top = stack_base + 2 * PAGE_SIZE;
    stack_top &= ~0xFUL;  /* 16 字节对齐 */

    /* 初始化 task_struct */
    tsk->state = TASK_RUNNING;
    tsk->flags = 0;
    tsk->stack = (void *)stack_base;
    tsk->pid = next_user_pid++;
    tsk->prio = 20;         /* nice=0 */
    tsk->mm = mm;

    /* 设置进程名 */
    for (i = 0; i < 15 && name[i]; i++)
        tsk->comm[i] = name[i];
    tsk->comm[i] = '\0';

    /* 设置调度实体 */
    tsk->se.load.weight = sched_prio_to_weight[tsk->prio];
    tsk->se.vruntime = 0;
    tsk->se.exec_start = 0;
    tsk->se.sum_exec_runtime = 0;
    tsk->se.on_rq = 0;
    tsk->se.run_node.rb_left = NULL;
    tsk->se.run_node.rb_right = NULL;
    tsk->se.run_node.__rb_parent_color = 0;

    /*
     * Step 3: 在内核栈顶构造 pt_regs
     *
     * 当 cpu_switch_to 恢复此进程时，sp = uregs 地址，
     * 然后 ret_to_user 执行 kernel_exit 0，恢复这些寄存器并 eret 到 EL0。
     *
     *  PSTATE = 0x0 → EL0t（SPSR_EL1 for eret to EL0）
     *    M[4:0] = 0b00000 = EL0t
     *    DAIF   = 0（中断使能）
     */
    uregs = (struct pt_regs *)(stack_top - PT_REGS_SIZE);

    /* 清零所有通用寄存器 */
    for (i = 0; i < 31; i++)
        uregs->regs[i] = 0;

    uregs->sp = user_sp;       /* SP_EL0：用户态栈指针 */
    uregs->pc = entry_pc;      /* ELR_EL1：用户态入口 */
    uregs->pstate = 0;         /* SPSR_EL1：EL0t, 中断使能 */

    /*
     * Step 4: 设置 cpu_context
     *
     * cpu_switch_to 恢复寄存器后 br x9（pc），跳转到 ret_to_user。
     * ret_to_user 从 sp（= uregs 地址）执行 kernel_exit 0。
     */
    tsk->thread.sp = (unsigned long)uregs;
    tsk->thread.pc = (unsigned long)ret_to_user;
    tsk->thread.x19 = 0;
    tsk->thread.x20 = 0;
    tsk->thread.x21 = 0;
    tsk->thread.x22 = 0;
    tsk->thread.x23 = 0;
    tsk->thread.x24 = 0;
    tsk->thread.x25 = 0;
    tsk->thread.x26 = 0;
    tsk->thread.x27 = 0;
    tsk->thread.x28 = 0;
    tsk->thread.fp = 0;

    /* Step 5: 加入 CFS 调度队列 */
    rq = this_rq();
    enqueue_entity(&rq->cfs, &tsk->se);

    boot_printk("[fork] Created user task: ");
    boot_printk(name);
    boot_printk(" (pid=");
    boot_printk_hex(tsk->pid);
    boot_printk(", entry=");
    boot_printk_hex(entry_pc);
    boot_printk(", sp=");
    boot_printk_hex(user_sp);
    boot_printk(")\n");

    return tsk;
}
