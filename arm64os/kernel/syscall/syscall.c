/* SPDX-License-Identifier: GPL-2.0 */
/*
 * arm64os/kernel/syscall/syscall.c
 *
 * 系统调用表与分发
 *
 * 参考：arch/arm64/kernel/syscall.c
 *       include/uapi/asm-generic/unistd.h
 *
 * Phase 5 实现：
 *   - do_el0_svc()：从 pt_regs 中读取 x8（syscall number），
 *     在 sys_call_table 中查找并调用对应的处理函数。
 *   - sys_write()：fd=1 时输出到 UART（最小化 write）
 *   - sys_exit()：标记进程为 TASK_DEAD 并调用 schedule()
 *
 * 系统调用号使用 ARM64 Linux 标准编号（asm-generic/unistd.h）：
 *   __NR_write = 64
 *   __NR_exit  = 93
 */

#include <linux/types.h>
#include <linux/sched.h>

/* ---- 错误码（简化版）---- */
#define ENOSYS      38      /* 无效系统调用号 */
#define EBADF       9       /* 错误的文件描述符 */
#define EFAULT      14      /* 错误的地址 */

/* ---- 系统调用号（ARM64 Linux ABI）---- */
#define __NR_write      64
#define __NR_exit       93
#define NR_SYSCALLS     256     /* 系统调用表大小 */

/*
 * pt_regs 结构体布局（与 entry.S 和 main.c 中的定义一致）
 *
 * 注意：此处前向声明，实际定义在 main.c 中。
 * Phase 5 通过 regs->regs[N] 访问各寄存器的值。
 */
struct pt_regs {
    unsigned long regs[31];     /* x0-x30 */
    unsigned long sp;           /* 用户态 SP_EL0 */
    unsigned long pc;           /* ELR_EL1 */
    unsigned long pstate;       /* SPSR_EL1 */
};

/* 外部函数 */
void boot_printk(const char *s);
void boot_printk_char(char c);
void boot_printk_hex(unsigned long val);
extern struct task_struct *current_task;
void schedule(void);

/*
 * ============================================================
 * sys_write - write 系统调用
 *
 * 简化版：仅支持 fd=1（stdout）→ PL011 UART 输出。
 * 不支持其他文件描述符（Phase 7 VFS 后完善）。
 *
 * 参数（ARM64 ABI）：
 *   x0 = fd
 *   x1 = buf（用户态缓冲区地址）
 *   x2 = count
 *
 * 返回值：写入的字节数，或负数错误码。
 *
 * 参考：fs/read_write.c ksys_write()
 * ============================================================
 */
static long sys_write(struct pt_regs *regs)
{
    int fd              = (int)regs->regs[0];
    const char *buf     = (const char *)regs->regs[1];
    size_t count        = (size_t)regs->regs[2];
    size_t i;

    /* Phase 5：仅支持 stdout（fd=1）和 stderr（fd=2）*/
    if (fd != 1 && fd != 2)
        return -(long)EBADF;

    /* 安全检查：buf 不能为 NULL */
    if (!buf)
        return -(long)EFAULT;

    /* 逐字符输出到 UART */
    for (i = 0; i < count; i++) {
        char c = buf[i];
        if (c == '\0')
            break;
        boot_printk_char(c);
    }

    return (long)i;
}

/*
 * ============================================================
 * sys_exit - exit 系统调用
 *
 * 终止当前进程：标记为 TASK_DEAD，调用 schedule() 切换走。
 * 不释放资源（Phase 5 简化版，无需清理页表等）。
 *
 * 参数：
 *   x0 = status（退出码，Phase 5 忽略）
 *
 * 不返回。
 *
 * 参考：kernel/exit.c do_exit()
 * ============================================================
 */
static long sys_exit(struct pt_regs *regs)
{
    int status = (int)regs->regs[0];

    boot_printk("[syscall] sys_exit(");
    boot_printk_hex((unsigned long)status);
    boot_printk(")\n");

    current_task->state = TASK_DEAD;
    schedule();

    /* 不应到达这里 */
    while (1)
        ;
    return 0;
}

/*
 * ============================================================
 * 系统调用表
 *
 * 函数指针数组，索引 = 系统调用号。
 * NULL 表示未实现（返回 -ENOSYS）。
 *
 * 参考：arch/arm64/kernel/syscall.c sys_call_table[]
 * ============================================================
 */
typedef long (*syscall_fn_t)(struct pt_regs *);

static const syscall_fn_t sys_call_table[NR_SYSCALLS] = {
    [__NR_write]    = sys_write,
    [__NR_exit]     = sys_exit,
    /* 其他调用号默认为 NULL → -ENOSYS */
};

/*
 * ============================================================
 * do_el0_svc - EL0 SVC 系统调用分发
 *
 * 由 entry.S 的 el0_svc 调用，传入 pt_regs 指针。
 *
 * 1. 从 regs->regs[8] 读取系统调用号（x8）
 * 2. 边界检查
 * 3. 在 sys_call_table 中查找处理函数
 * 4. 调用处理函数，将返回值写回 regs->regs[0]
 *
 * 参考：arch/arm64/kernel/syscall.c do_el0_svc()
 * ============================================================
 */
void do_el0_svc(struct pt_regs *regs)
{
    unsigned long nr = regs->regs[8];   /* x8 = syscall number */
    syscall_fn_t fn;

    /* 边界检查 */
    if (nr >= NR_SYSCALLS) {
        regs->regs[0] = (unsigned long)(-(long)ENOSYS);
        return;
    }

    fn = sys_call_table[nr];
    if (!fn) {
        regs->regs[0] = (unsigned long)(-(long)ENOSYS);
        return;
    }

    /* 调用系统调用处理函数，返回值存入 x0 */
    regs->regs[0] = (unsigned long)fn(regs);
}
