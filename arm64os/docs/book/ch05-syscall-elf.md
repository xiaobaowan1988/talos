# Phase 5：系统调用 + ELF 加载器

## 知识来源总览

- **ARMv8-A 异常模型**：约 30%（SVC 指令、ESR_EL1.EC 分发、SPSR/ELR）
- **ELF 规范 (System V ABI)**：约 30%（ELF header、Program header、PT_LOAD）
- **Linux 系统调用 ABI**：约 20%（x8=syscall number、x0=return value）
- **Phase 5 文档**：约 20%

## 系统调用入口

### el0_sync 异常分发

```c
void el0_sync_handler(struct pt_regs *regs) {
    unsigned long esr = read_sysreg(esr_el1);
    unsigned int ec = (esr >> 26) & 0x3F;   /* Exception Class */

    switch (ec) {
    case 0x15:  /* EC=0x15: SVC from AArch64 */
        do_el0_svc(regs);
        break;
    case 0x20:  /* Instruction Abort from lower EL */
        do_page_fault(regs, esr);
        break;
    case 0x24:  /* Data Abort from lower EL */
        do_page_fault(regs, esr);
        break;
    }
}
```

**来源：ARMv8-A ARM, Table D1-6 Encoding of EC field**。

用户态执行 `svc #0` 触发同步异常 → 向量表 sync_el0_64 → `el0_sync_handler`。ESR_EL1（Exception Syndrome Register）的 EC 字段（bit[31:26]）编码异常类型。

EC=0x15 是 "SVC instruction execution in AArch64 state"。这是系统调用的标准路径。

### do_el0_svc 系统调用分发

```c
typedef long (*syscall_fn_t)(struct pt_regs *regs);

static syscall_fn_t syscall_table[] = {
    [0] = sys_read,
    [1] = sys_write,
    [2] = sys_openat,
    [3] = sys_close,
    [60] = sys_exit,
    [220] = sys_clone,
    /* ... */
};

void do_el0_svc(struct pt_regs *regs) {
    unsigned long scno = regs->regs[8];  /* x8 = syscall number */

    if (scno < NR_SYSCALLS && syscall_table[scno])
        regs->regs[0] = syscall_table[scno](regs);
    else
        regs->regs[0] = -ENOSYS;
}
```

**来源：Linux ARM64 syscall ABI**。

x8 = 系统调用号（ARM64 特有，x86-64 用 rax）。x0-x5 = 参数。返回值写入 x0。

**为什么通过 pt_regs 传参？** pt_regs 就是内核栈上保存的寄存器快照。修改 `regs->regs[0]` 后，`kernel_exit` 恢复寄存器时 x0 就是新值——用户态看到的返回值。

### sys_write 实现

```c
ssize_t sys_write(struct pt_regs *regs) {
    int fd = regs->regs[0];
    const char __user *buf = (const char *)regs->regs[1];
    size_t count = regs->regs[2];

    if (!access_ok(buf, count))
        return -EFAULT;

    /* fd=1 → stdout → UART */
    if (fd == 1) {
        for (size_t i = 0; i < count; i++)
            uart_putc(buf[i]);
        return count;
    }
    return -EBADF;
}
```

**`access_ok(buf, count)`**：验证用户态指针合法。检查 buf 到 buf+count 的范围是否在用户空间（低地址，TTBR0 管理的区域）。如果用户传入内核地址，必须拒绝——否则是信息泄露漏洞。

## ELF 加载器

### ELF Header 验证

```c
int load_elf_binary(const char *elf_data) {
    Elf64_Ehdr *ehdr = (Elf64_Ehdr *)elf_data;

    /* 魔数验证 */
    if (ehdr->e_ident[0] != 0x7f ||
        ehdr->e_ident[1] != 'E'  ||
        ehdr->e_ident[2] != 'L'  ||
        ehdr->e_ident[3] != 'F')
        return -ENOEXEC;

    /* 类型检查 */
    if (ehdr->e_ident[4] != ELFCLASS64)    /* 64位 */
        return -ENOEXEC;
    if (ehdr->e_ident[5] != ELFDATA2LSB)   /* 小端 */
        return -ENOEXEC;
    if (ehdr->e_machine != EM_AARCH64)     /* ARM64 */
        return -ENOEXEC;
```

**来源：System V ABI, ELF specification**。

`0x7f 'E' 'L' 'F'`：ELF 魔数。ELFCLASS64=2（64位），ELFDATA2LSB=1（小端），EM_AARCH64=183。

### PT_LOAD 段映射

```c
    Elf64_Phdr *phdr = (Elf64_Phdr *)(elf_data + ehdr->e_phoff);

    for (int i = 0; i < ehdr->e_phnum; i++) {
        if (phdr[i].p_type != PT_LOAD)
            continue;

        unsigned long vaddr = phdr[i].p_vaddr;
        unsigned long filesz = phdr[i].p_filesz;
        unsigned long memsz = phdr[i].p_memsz;

        /* 分配用户页并映射到 TTBR0 页表 */
        map_user_pages(vaddr, memsz, elf_to_pte_flags(phdr[i].p_flags));

        /* 复制文件内容 */
        memcpy((void *)vaddr, elf_data + phdr[i].p_offset, filesz);

        /* memsz > filesz 的部分是 BSS，已被清零 */
        if (memsz > filesz)
            memset((void *)(vaddr + filesz), 0, memsz - filesz);
    }
```

**PT_LOAD（type=1）**：需要加载到内存的段。ELF 可执行文件通常有两个 PT_LOAD 段：
- `.text + .rodata`（可读可执行）
- `.data + .bss`（可读可写）

**p_filesz vs p_memsz**：`p_memsz >= p_filesz`。差值就是 BSS 段（未初始化全局变量，在文件中不占空间，内存中需要清零）。

### 进入用户态

```c
    /* 设置用户页表 */
    write_sysreg(user_pgd, ttbr0_el1);
    tlb_flush_all();

    /* 构造返回帧 */
    struct pt_regs *regs = task_pt_regs(current);
    regs->pc = ehdr->e_entry;    /* ELF 入口地址 */
    regs->pstate = 0;            /* EL0t, DAIF全开（允许中断）*/
    regs->sp = USER_STACK_TOP;

    /* eret 返回用户态 */
}
```

**`pstate = 0`**：SPSR_EL1 = 0 意味着：
- bit[3:0] = 0000 = EL0t（用户态，使用 SP_EL0）
- bit[9:6] = 0000 = DAIF 全部不屏蔽（用户态可以被中断）

`eret` 后处理器切换到 EL0，PC = `e_entry`，SP = 用户态栈顶。

## fork 与 "返回两次"

```c
void copy_thread(struct task_struct *p, unsigned long clone_flags,
                 unsigned long stack_start) {
    struct pt_regs *childregs = task_pt_regs(p);

    *childregs = *task_pt_regs(current);  /* 复制父进程的寄存器 */
    childregs->regs[0] = 0;               /* 子进程的 fork 返回值 = 0 */

    p->cpu_context.pc = (unsigned long)ret_from_fork;
    p->cpu_context.sp = (unsigned long)childregs;
}
```

**"返回两次"机制**：
1. 父进程的 `fork()` 正常返回子进程 PID
2. 子进程被调度时，`cpu_switch_to` 恢复 cpu_context，`ret` 跳到 `ret_from_fork`
3. `ret_from_fork` 调用 `kernel_exit`，恢复 pt_regs（其中 x0=0）→ `eret` → 子进程的用户态

子进程看到 `fork()` 返回 0，父进程看到返回子 PID。
