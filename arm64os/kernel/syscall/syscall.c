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
 * Phase 7 新增：
 *   - sys_openat()：通过 VFS 打开/创建文件
 *   - sys_close()：通过 VFS 关闭文件描述符
 *   - sys_read()：通过 VFS 读取文件
 *   - sys_write() 更新：fd=1/2 仍输出 UART，其他 fd 走 VFS
 *
 * 系统调用号使用 ARM64 Linux 标准编号（asm-generic/unistd.h）：
 *   __NR_openat = 56
 *   __NR_close  = 57
 *   __NR_read   = 63
 *   __NR_write  = 64
 *   __NR_exit   = 93
 */

#include <linux/types.h>
#include <linux/sched.h>
#include <linux/fs.h>

/* ---- 错误码（简化版）---- */
#define ENOSYS      38      /* 无效系统调用号 */
#define EBADF       9       /* 错误的文件描述符 */
#define EFAULT      14      /* 错误的地址 */

/* ---- 系统调用号（ARM64 Linux ABI）---- */
#define __NR_openat     56
#define __NR_close      57
#define __NR_read       63
#define __NR_write      64
#define __NR_exit       93
#define NR_SYSCALLS     256     /* 系统调用表大小 */

/*
 * pt_regs 结构体布局（与 entry.S 和 main.c 中的定义一致）
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

/* VFS 接口（fs/vfs/file.c） */
int do_sys_open(struct files_struct *files, const char *pathname,
                int flags, unsigned int mode);
int do_sys_close(struct files_struct *files, int fd);
ssize_t vfs_read(struct file *filp, char *buf, size_t count);
ssize_t vfs_write(struct file *filp, const char *buf, size_t count);
struct file *fget(struct files_struct *files, int fd);

/* 全局初始文件描述符表 */
extern struct files_struct init_files;

/*
 * ============================================================
 * sys_openat - openat 系统调用
 *
 * 参数（ARM64 ABI）：
 *   x0 = dirfd（AT_FDCWD = -100 表示相对于当前目录）
 *   x1 = pathname（用户态路径字符串地址）
 *   x2 = flags（O_RDONLY, O_WRONLY, O_CREAT 等）
 *   x3 = mode（创建权限，仅 O_CREAT 时有效）
 *
 * 返回值：文件描述符（>= 0），或负数错误码。
 *
 * Phase 7 简化：忽略 dirfd，仅支持绝对路径。
 *
 * 参考：fs/open.c sys_openat()
 * ============================================================
 */
static long sys_openat(struct pt_regs *regs)
{
    /* int dirfd = (int)regs->regs[0]; */  /* Phase 7: 忽略 */
    const char *pathname = (const char *)regs->regs[1];
    int flags            = (int)regs->regs[2];
    unsigned int mode    = (unsigned int)regs->regs[3];
    struct files_struct *files;

    if (!pathname)
        return -(long)EFAULT;

    /* 获取当前进程的文件描述符表 */
    files = current_task->files;
    if (!files)
        files = &init_files;

    return (long)do_sys_open(files, pathname, flags, mode);
}

/*
 * ============================================================
 * sys_close - close 系统调用
 *
 * 参数：
 *   x0 = fd
 *
 * 返回 0 成功，负数错误码。
 *
 * 参考：fs/open.c sys_close()
 * ============================================================
 */
static long sys_close(struct pt_regs *regs)
{
    int fd = (int)regs->regs[0];
    struct files_struct *files;

    files = current_task->files;
    if (!files)
        files = &init_files;

    return (long)do_sys_close(files, fd);
}

/*
 * ============================================================
 * sys_read - read 系统调用
 *
 * 参数：
 *   x0 = fd
 *   x1 = buf（用户态缓冲区地址）
 *   x2 = count
 *
 * 返回实际读取的字节数，或负数错误码。
 *
 * 参考：fs/read_write.c ksys_read()
 * ============================================================
 */
static long sys_read(struct pt_regs *regs)
{
    int fd          = (int)regs->regs[0];
    char *buf       = (char *)regs->regs[1];
    size_t count    = (size_t)regs->regs[2];
    struct files_struct *files;
    struct file *filp;

    if (!buf)
        return -(long)EFAULT;

    files = current_task->files;
    if (!files)
        files = &init_files;

    filp = fget(files, fd);
    if (!filp)
        return -(long)EBADF;

    return (long)vfs_read(filp, buf, count);
}

/*
 * ============================================================
 * sys_write - write 系统调用
 *
 * Phase 7 更新：
 *   - fd=1/2（stdout/stderr）：直接输出到 UART（保持 Phase 5 行为）
 *   - 其他 fd：通过 VFS 写入
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
    struct files_struct *files;
    struct file *filp;

    /* 安全检查：buf 不能为 NULL */
    if (!buf)
        return -(long)EFAULT;

    /* stdout/stderr：直接输出到 UART（保持 Phase 5 兼容）*/
    if (fd == 1 || fd == 2) {
        for (i = 0; i < count; i++) {
            char c = buf[i];
            if (c == '\0')
                break;
            boot_printk_char(c);
        }
        return (long)i;
    }

    /* 其他 fd：通过 VFS */
    files = current_task->files;
    if (!files)
        files = &init_files;

    filp = fget(files, fd);
    if (!filp)
        return -(long)EBADF;

    return (long)vfs_write(filp, buf, count);
}

/*
 * ============================================================
 * sys_exit - exit 系统调用
 *
 * 终止当前进程：标记为 TASK_DEAD，调用 schedule() 切换走。
 *
 * 参数：
 *   x0 = status（退出码）
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
 * Phase 7 新增：openat(56), close(57), read(63)
 *
 * 参考：arch/arm64/kernel/syscall.c sys_call_table[]
 * ============================================================
 */
typedef long (*syscall_fn_t)(struct pt_regs *);

static const syscall_fn_t sys_call_table[NR_SYSCALLS] = {
    [__NR_openat]   = sys_openat,
    [__NR_close]    = sys_close,
    [__NR_read]     = sys_read,
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
