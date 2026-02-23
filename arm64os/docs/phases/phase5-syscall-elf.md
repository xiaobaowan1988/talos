# Phase 5：系统调用框架 + ELF加载器

## 参考内核文件

```
arch/arm64/kernel/entry.S           # SVC异常入口、syscall分发
arch/arm64/kernel/syscall.c         # do_el0_svc、系统调用表
arch/arm64/include/asm/unistd.h     # ARM64系统调用号定义
include/uapi/asm-generic/unistd.h   # 通用系统调用号
fs/binfmt_elf.c                     # ELF可执行文件加载
include/uapi/linux/elf.h            # ELF文件格式定义
kernel/fork.c                       # do_fork、copy_process
arch/arm64/kernel/process.c         # copy_thread（新进程栈帧）
```

---

## 5.1 ARM64系统调用机制（参考 entry.S + syscall.c）

用户态通过 `svc #0` 指令陷入内核，触发 EL0 Synchronous 异常。
异常类型由 `ESR_EL1.EC` 字段判断：`EC=0x15` 表示 AArch64 SVC。

```
系统调用约定（ARM64 Linux ABI）：
  x8  = 系统调用号（syscall number）
  x0  = 第1个参数 / 返回值
  x1  = 第2个参数
  x2  = 第3个参数
  x3  = 第4个参数
  x4  = 第5个参数
  x5  = 第6个参数
```

```asm
/* 参考 arch/arm64/kernel/entry.S */
/* EL0 SVC处理路径 */
el0t_64_sync_handler:
    mrs     x25, esr_el1
    lsr     x24, x25, #ESR_ELx_EC_SHIFT
    cmp     x24, #ESR_ELx_EC_SVC64
    b.eq    el0_svc

el0_svc:
    /* 从 pt_regs 中取出系统调用号（保存在 x8）*/
    ldr     x8, [sp, #S_X8]
    /* 调用 C 层系统调用分发 */
    bl      do_el0_svc
```

```c
/* 参考 arch/arm64/kernel/syscall.c */

/* 系统调用表：函数指针数组 */
typedef long (*syscall_fn_t)(const struct pt_regs *);

static const syscall_fn_t sys_call_table[] = {
    [__NR_read]     = sys_read,
    [__NR_write]    = sys_write,
    [__NR_openat]   = sys_openat,
    [__NR_close]    = sys_close,
    [__NR_mmap]     = sys_mmap,
    [__NR_exit]     = sys_exit,
    [__NR_execve]   = sys_execve,
    [__NR_fork]     = sys_fork,
    [__NR_wait4]    = sys_wait4,
    /* ... 共约300个系统调用 */
};

void do_el0_svc(struct pt_regs *regs) {
    unsigned long nr = regs->regs[8];

    /* 边界检查 */
    if (nr >= ARRAY_SIZE(sys_call_table)) {
        regs->regs[0] = -ENOSYS;
        return;
    }

    /* 调用实际系统调用处理函数 */
    regs->regs[0] = sys_call_table[nr](regs);
}
```

## 5.2 关键系统调用实现

### write 系统调用（最简单，用于调试输出）

```c
/* 参考 fs/read_write.c */
long sys_write(const struct pt_regs *regs) {
    int fd        = (int)regs->regs[0];
    const char *buf = (const char *)regs->regs[1];
    size_t count  = (size_t)regs->regs[2];

    struct file *f = fget(fd);
    if (!f) return -EBADF;

    ssize_t ret = f->f_op->write(f, buf, count, &f->f_pos);
    fput(f);
    return ret;
}
```

### mmap 系统调用（关键：用户空间内存映射）

```c
/* 参考 arch/arm64/kernel/sys.c + mm/mmap.c */
long sys_mmap(const struct pt_regs *regs) {
    unsigned long addr  = regs->regs[0];
    unsigned long len   = regs->regs[1];
    int prot            = (int)regs->regs[2];
    int flags           = (int)regs->regs[3];
    int fd              = (int)regs->regs[4];
    unsigned long off   = regs->regs[5];

    return do_mmap(addr, len, prot, flags, fd, off);
}
```

## 5.3 ELF文件格式（参考 include/uapi/linux/elf.h）

```c
/* ELF64头部（64字节）*/
typedef struct {
    unsigned char e_ident[16]; /* 魔数: "\x7fELF" + class/data/version... */
    uint16_t      e_type;      /* ET_EXEC=2(可执行), ET_DYN=3(共享库) */
    uint16_t      e_machine;   /* EM_AARCH64=183 */
    uint32_t      e_version;
    uint64_t      e_entry;     /* 入口点虚拟地址 */
    uint64_t      e_phoff;     /* Program Header Table偏移 */
    uint64_t      e_shoff;     /* Section Header Table偏移 */
    uint32_t      e_flags;
    uint16_t      e_ehsize;    /* ELF头大小（=64）*/
    uint16_t      e_phentsize; /* 每个Program Header大小（=56）*/
    uint16_t      e_phnum;     /* Program Header数量 */
    /* ... */
} Elf64_Ehdr;

/* Program Header（段描述符，56字节）*/
typedef struct {
    uint32_t p_type;    /* PT_LOAD=1, PT_DYNAMIC=2, PT_INTERP=3 */
    uint32_t p_flags;   /* PF_R=4, PF_W=2, PF_X=1 */
    uint64_t p_offset;  /* 段在文件中的偏移 */
    uint64_t p_vaddr;   /* 段的虚拟地址 */
    uint64_t p_paddr;   /* 物理地址（通常同vaddr）*/
    uint64_t p_filesz;  /* 段在文件中的大小 */
    uint64_t p_memsz;   /* 段在内存中的大小（>=filesz，多余部分清零=BSS）*/
    uint64_t p_align;   /* 对齐要求 */
} Elf64_Phdr;
```

## 5.4 ELF加载流程（参考 fs/binfmt_elf.c load_elf_binary）

```c
/* 参考 fs/binfmt_elf.c */
int load_elf_binary(struct linux_binprm *bprm) {
    Elf64_Ehdr elf_ex;
    Elf64_Phdr *elf_phdata;

    /* Step 1: 读取并验证ELF头 */
    read_from_file(bprm->file, 0, &elf_ex, sizeof(elf_ex));
    if (memcmp(elf_ex.e_ident, ELFMAG, 4) != 0)
        return -ENOEXEC;
    if (elf_ex.e_machine != EM_AARCH64)
        return -ENOEXEC;

    /* Step 2: 读取所有Program Header */
    elf_phdata = kmalloc(elf_ex.e_phnum * sizeof(Elf64_Phdr));
    read_from_file(bprm->file, elf_ex.e_phoff, elf_phdata,
                   elf_ex.e_phnum * sizeof(Elf64_Phdr));

    /* Step 3: 遍历PT_LOAD段，映射到进程地址空间 */
    for (int i = 0; i < elf_ex.e_phnum; i++) {
        Elf64_Phdr *eppnt = &elf_phdata[i];
        if (eppnt->p_type != PT_LOAD)
            continue;

        /* 计算保护标志 */
        int prot = 0;
        if (eppnt->p_flags & PF_R) prot |= PROT_READ;
        if (eppnt->p_flags & PF_W) prot |= PROT_WRITE;
        if (eppnt->p_flags & PF_X) prot |= PROT_EXEC;

        /* 映射文件内容到虚拟地址 */
        elf_map(bprm->file, eppnt->p_vaddr, eppnt, prot,
                MAP_FIXED | MAP_PRIVATE);

        /* 如果 p_memsz > p_filesz，多余部分清零（BSS段）*/
        if (eppnt->p_memsz > eppnt->p_filesz) {
            unsigned long bss_start = eppnt->p_vaddr + eppnt->p_filesz;
            unsigned long bss_len   = eppnt->p_memsz - eppnt->p_filesz;
            memset((void *)bss_start, 0, bss_len);
        }
    }

    /* Step 4: 建立用户栈，放入 argc/argv/envp/auxv */
    setup_arg_pages(bprm);
    create_elf_tables(bprm, &elf_ex);  /* 写入auxv（AT_ENTRY, AT_PHDR等）*/

    /* Step 5: 设置进程入口点和栈指针 */
    start_thread(regs, elf_ex.e_entry, bprm->p);
    return 0;
}
```

## 5.5 fork + execve 组合（进程创建）

```c
/* 参考 kernel/fork.c */
long do_fork(unsigned long clone_flags, unsigned long stack_start) {
    struct task_struct *p;

    /* 1. 复制 task_struct */
    p = copy_process(clone_flags, stack_start);

    /* 2. 为新进程分配 pid */
    pid_t pid = alloc_pid();
    p->pid = pid;

    /* 3. 将新进程加入调度队列 */
    wake_up_new_task(p);

    return pid;  /* 父进程返回子进程pid */
}

/* 参考 arch/arm64/kernel/process.c */
int copy_thread(struct task_struct *p, unsigned long stack_start) {
    struct pt_regs *childregs = task_pt_regs(p);

    /* 子进程的 pt_regs：x0=0（fork返回0给子进程）*/
    *childregs = *current_pt_regs();
    childregs->regs[0] = 0;

    /* 子进程从 ret_from_fork 开始执行 */
    p->thread.cpu_context.pc = (unsigned long)ret_from_fork;
    p->thread.cpu_context.sp = (unsigned long)childregs;
    return 0;
}
```

## 5.6 验证方法

```c
/* 最小用户态程序验证 */
/* 编译: aarch64-linux-gnu-gcc -static -o hello hello.c */

/* hello.c */
#include <sys/syscall.h>
void _start(void) {
    /* write(1, "Hello\n", 6) */
    register long x8 asm("x8") = __NR_write;
    register long x0 asm("x0") = 1;
    register const char *x1 asm("x1") = "Hello, ARM64!\n";
    register long x2 asm("x2") = 14;
    asm volatile("svc #0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x8));

    /* exit(0) */
    x8 = __NR_exit; x0 = 0;
    asm volatile("svc #0");
}

/* 预期输出：Hello, ARM64! */
```

## 5.7 本阶段产出文件

```
arm64os/
├── arch/arm64/kernel/
│   ├── entry.S            ← 更新：el0_sync 区分 SVC 与其他异常
│   │                         添加 kernel_entry_from_el0 / kernel_exit_to_el0
│   │                         宏（保存/恢复 SP_EL0 用户栈指针）
│   └── process.S          ← 更新：context_switch 中切换 TTBR0（用户页表）
├── arch/arm64/mm/
│   └── mmu.c              ← 更新：添加用户页表创建/映射接口
│                              create_user_pgd()、map_user_page()
├── arch/arm64/include/asm/
│   └── pgtable.h          ← 更新：添加 2MB Block / 4KB Page 描述符构建宏
├── include/linux/
│   ├── sched.h            ← 更新：添加 mm_struct、task_struct.mm 字段
│   └── elf.h              ← 新增：ELF64 文件格式结构体定义
├── kernel/
│   ├── syscall/
│   │   └── syscall.c      ← 新增：sys_call_table + do_el0_svc 分发
│   │                         实现 sys_write（UART）、sys_exit
│   └── fork.c             ← 新增：do_fork、copy_process（简化版）
├── fs/
│   └── binfmt_elf.c       ← 新增：ELF 加载器（从内存解析 ELF 映像）
│                              load_elf_binary()：解析段、映射用户页
├── userspace/
│   ├── init.S             ← 新增：最小 init 进程汇编源码
│   ├── user.ld            ← 新增：用户程序链接脚本（基址 0x00400000）
│   └── init_blob.S        ← 新增：.incbin 嵌入 init.bin 到内核 .rodata
└── kernel/main.c          ← 更新：Phase 5 初始化 + 验证流程
```

### 设计说明（与原始草案的差异）

1. **不使用单独的 `syscall.S`**：SVC 入口路径直接在 `entry.S` 的
   `el0_sync` 中通过 ESR_EL1.EC 判断实现（与 Linux 主线一致）。
   C 层分发在 `kernel/syscall/syscall.c` 中的 `do_el0_svc()` 完成。

2. **用户页表基础设施**：Phase 5 需要 EL0 运行的进程拥有独立的
   TTBR0 页表。`mmu.c` 新增 `create_user_pgd()` 创建包含内核恒等映射
   + 用户代码/栈映射的 L0-L3 页表；`process.S` 在上下文切换时更新 TTBR0。

3. **用户程序嵌入方式**：Phase 5 尚无文件系统，init 进程以二进制
   形式嵌入内核 `.rodata` 段。构建流程：
   `init.S → init.elf → init.bin → .incbin → 内核链接`。
   ELF 加载器从内存中解析该映像。

4. **用户虚拟地址布局**：
   - 代码段：`0x00400000`（标准用户文本段起始地址）
   - 用户栈顶：`0x00800000`
   - 内核恒等映射 `[0x40000000, 0x80000000)` 以 AP=00（仅内核可访问）保留
