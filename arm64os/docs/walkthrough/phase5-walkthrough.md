# Phase 5 Walkthrough: 系统调用框架 + ELF 加载器

> **目标**：让内核加载并运行用户程序，用户程序通过系统调用与内核交互。
> **最终效果**：用户程序输出 `Hello from user space!` 然后 `exit(0)` 退出。

---

## 5.1 用户态 vs 内核态

到 Phase 4 为止，所有代码都在 EL1（内核态）运行。但真正的操作系统需要：

```
  ┌─────────────┐
  │   用户程序   │  EL0 — 受限权限，不能直接访问硬件
  ├─────────────┤
  │  系统调用    │  SVC #0 — 陷入内核的唯一合法入口
  ├─────────────┤
  │   内核       │  EL1 — 完全权限，管理硬件和资源
  └─────────────┘
```

**ARM64 权限隔离**：

| | EL0（用户态） | EL1（内核态） |
|---|---|---|
| 内存访问 | 只能访问用户页表映射的地址 | 可以访问所有内存 |
| 系统寄存器 | 不能读写大多数系统寄存器 | 完全访问 |
| 特权指令 | 不能执行（会触发异常） | 可以执行 |
| 进入方式 | `eret` 从 EL1 返回 | `SVC` 从 EL0 陷入 |

---

## 5.2 系统调用机制

### ARM64 ABI

```
  用户程序:
    x8  = 系统调用号（如 64 = write）
    x0  = 参数 1（如 fd）
    x1  = 参数 2（如 buf 指针）
    x2  = 参数 3（如 count）
    x3-x5 = 参数 4-6
    SVC #0              ← 触发同步异常，陷入 EL1

  返回后:
    x0  = 返回值（或负数表示错误）
```

### 从硬件到 C 代码的路径

```
  用户程序执行 SVC #0
      │
      ▼
  CPU 自动: 保存 PC→ELR_EL1, 保存 PSTATE→SPSR_EL1
            切换到 EL1, 跳到 VBAR_EL1 + 0x400 (el0_sync)
      │
      ▼
  el0_sync (entry.S):
      kernel_entry 0          ← 保存所有寄存器到 pt_regs
      mrs x25, esr_el1        ← 读取异常原因
      lsr x24, x25, #26       ← 提取 EC（Exception Class）
      cmp x24, #0x15          ← EC=0x15 是 SVC
      b.eq el0_svc            ← 是系统调用！
      │
      ▼
  el0_svc:
      msr daifclr, #2         ← 使能 IRQ（允许抢占）
      mov x0, sp              ← x0 = pt_regs 指针
      bl do_el0_svc            ← 调用 C 函数
      msr daifset, #2         ← 关 IRQ
      kernel_exit 0            ← 恢复寄存器，eret 回 EL0
```

### C 层系统调用分发

```c
/* 系统调用表 */
typedef long (*syscall_fn_t)(struct pt_regs *regs);
static syscall_fn_t sys_call_table[NR_SYSCALLS];

void do_el0_svc(struct pt_regs *regs)
{
    unsigned long scno = regs->regs[8];  /* x8 = 系统调用号 */

    if (scno < NR_SYSCALLS && sys_call_table[scno]) {
        regs->regs[0] = sys_call_table[scno](regs);  /* 返回值写回 x0 */
    } else {
        regs->regs[0] = -1;  /* 未知系统调用 */
    }
}
```

### Phase 5 实现的系统调用

```c
/* 64: write(fd, buf, count) */
static long sys_write(struct pt_regs *regs)
{
    int fd = (int)regs->regs[0];
    const char *buf = (const char *)regs->regs[1];
    size_t count = (size_t)regs->regs[2];

    if (fd == 1 || fd == 2) {  /* stdout / stderr */
        for (size_t i = 0; i < count; i++)
            boot_printk_char(buf[i]);  /* 输出到 UART */
        return (long)count;
    }
    return -1;
}

/* 93: exit(status) */
static long sys_exit(struct pt_regs *regs)
{
    current_task->state = TASK_DEAD;  /* 标记进程死亡 */
    schedule();                        /* 调度到其他进程 */
    return 0;  /* 不会到这里 */
}
```

---

## 5.3 ELF 加载器

### ELF 是什么？

ELF（Executable and Linkable Format）是 Linux 下的可执行文件格式。它告诉内核：

- 代码放在虚拟地址哪里
- 数据放在虚拟地址哪里
- 从哪个地址开始执行

### ELF64 文件结构

```
  ┌────────────────┐ offset 0
  │   ELF Header   │  魔数、架构、入口点、Program Header 位置
  ├────────────────┤
  │ Program Headers│  描述要加载的段（PT_LOAD）
  ├────────────────┤
  │   .text        │  代码段（可执行）
  ├────────────────┤
  │   .rodata      │  只读数据
  ├────────────────┤
  │   .data        │  可读写数据
  ├────────────────┤
  │   .bss         │  未初始化数据（文件中不占空间）
  └────────────────┘
```

### ELF Header 验证

```c
int load_elf_binary(const void *elf_data, size_t elf_size,
                    unsigned long pgd_phys, unsigned long *entry_out)
{
    Elf64_Ehdr *ehdr = (Elf64_Ehdr *)elf_data;

    /* 验证 ELF 魔数 */
    if (ehdr->e_ident[0] != 0x7f ||
        ehdr->e_ident[1] != 'E'  ||
        ehdr->e_ident[2] != 'L'  ||
        ehdr->e_ident[3] != 'F')
        return -1;

    /* 验证 64 位 + 小端 + ARM64 */
    if (ehdr->e_ident[4] != 2 ||   /* ELFCLASS64 */
        ehdr->e_ident[5] != 1 ||   /* ELFDATA2LSB */
        ehdr->e_machine != 183)    /* EM_AARCH64 */
        return -1;

    *entry_out = ehdr->e_entry;     /* 用户程序入口地址 */
```

### 加载 PT_LOAD 段

```c
    Elf64_Phdr *phdr = (Elf64_Phdr *)((char *)elf_data + ehdr->e_phoff);

    for (int i = 0; i < ehdr->e_phnum; i++) {
        if (phdr[i].p_type != PT_LOAD)
            continue;

        /* 逐页加载 */
        unsigned long vaddr = phdr[i].p_vaddr;
        unsigned long filesz = phdr[i].p_filesz;  /* 文件中的大小 */
        unsigned long memsz = phdr[i].p_memsz;    /* 内存中的大小 */

        for (unsigned long off = 0; off < memsz; off += PAGE_SIZE) {
            /* 分配物理页 */
            struct page *page = alloc_pages(0);
            unsigned long pa = (unsigned long)page_address(page);

            /* 拷贝文件内容 */
            if (off < filesz) {
                size_t copy_len = (filesz - off > PAGE_SIZE)
                                ? PAGE_SIZE : filesz - off;
                memcpy((void *)pa,
                       (char *)elf_data + phdr[i].p_offset + off,
                       copy_len);
            }

            /* BSS 部分清零（memsz > filesz 的部分） */

            /* 映射到用户页表 */
            unsigned long attrs = (phdr[i].p_flags & PF_X)
                                ? PD_USER_EXEC : PD_USER_DATA;
            map_user_page(pgd_phys, vaddr + off, pa, attrs);
        }
    }
    return 0;
}
```

**映射后的用户地址空间**：

```
  0x00000000 ┌──────────────┐
             │   (未映射)    │
  0x00400000 ├──────────────┤
             │  .text       │ ← ELF 代码段（可执行）
             │  .rodata     │
             ├──────────────┤
             │  .data       │ ← 可读写
             │  .bss        │
             ├──────────────┤
             │   ...        │
  0x007F0000 ├──────────────┤
             │  用户栈      │ ← 16KB，向下增长
  0x00800000 └──────────────┘ ← USER_STACK_TOP
```

---

## 5.4 用户页表

Phase 2 只有恒等映射（内核用）。Phase 5 需要为每个用户进程创建独立的页表。

### create_user_pgd()

```c
unsigned long create_user_pgd(void)
{
    /* 分配新的 PGD（L0 页表） */
    struct page *pgd_page = alloc_pages(0);
    unsigned long *pgd = page_address(pgd_page);

    /* 复制内核映射（PGD[0] = 内核恒等映射） */
    pgd[0] = init_pgd[0];

    /* PGD[1-511] 清零（用户空间，按需映射） */

    return (unsigned long)pgd;
}
```

### map_user_page() — 建立用户页映射

```c
int map_user_page(unsigned long pgd_phys, unsigned long va,
                  unsigned long pa, unsigned long attrs)
{
    /* 计算各级索引 */
    int l0_idx = (va >> 39) & 0x1FF;
    int l1_idx = (va >> 30) & 0x1FF;
    int l2_idx = (va >> 21) & 0x1FF;
    int l3_idx = (va >> 12) & 0x1FF;

    /* 逐级查找/创建页表 */
    /* L0[l0_idx] → L1 表 (自动分配) */
    /* L1[l1_idx] → L2 表 (自动分配) */
    /* L2[l2_idx] → L3 表 (自动分配) */
    /* L3[l3_idx] = pa | attrs | PTE_VALID */

    /* TLB 失效 */
    __asm__ volatile("tlbi vale1is, %0" :: "r"(va >> 12));
    __asm__ volatile("dsb ish; isb");

    return 0;
}
```

---

## 5.5 创建用户进程

```c
struct task_struct *create_user_task(unsigned long pgd_phys,
                                     unsigned long entry_pc,
                                     unsigned long user_sp,
                                     const char *name)
{
    struct task_struct *tsk = alloc_task();

    /* 设置用户地址空间 */
    tsk->mm = alloc_mm();
    tsk->mm->pgd = pgd_phys;

    /* 分配内核栈 */
    struct page *kstack = alloc_pages(1);  /* 8KB */
    tsk->stack = page_address(kstack);
    unsigned long stack_top = (unsigned long)tsk->stack + 2 * PAGE_SIZE;

    /* 在内核栈顶放置 pt_regs */
    struct pt_regs *regs = (struct pt_regs *)(stack_top - sizeof(struct pt_regs));

    /* 用户态初始寄存器 */
    for (int i = 0; i < 31; i++)
        regs->regs[i] = 0;          /* x0-x30 = 0 */
    regs->sp = user_sp;              /* 用户栈指针 */
    regs->pc = entry_pc;             /* 用户程序入口 */
    regs->pstate = 0;                /* EL0t, 中断开启 */

    /* 设置 cpu_context */
    tsk->thread.sp = (unsigned long)regs;   /* 内核栈 SP 指向 pt_regs */
    tsk->thread.pc = (unsigned long)ret_to_user;  /* 首次调度走 ret_to_user */

    /* 加入调度器 */
    enqueue_entity(&runqueue.cfs, &tsk->se);

    return tsk;
}
```

### 首次调度到用户进程的流程

```
  schedule() → cpu_switch_to(idle, init)
      │
      ▼
  cpu_switch_to 恢复 init 的 cpu_context:
    SP = pt_regs 地址
    PC = ret_to_user
      │
      ▼
  ret_to_user (entry.S):
    kernel_exit 0         ← 从 pt_regs 恢复用户寄存器
      │
      ├─ 恢复 SP_EL0 = user_sp
      ├─ 恢复 ELR_EL1 = entry_pc
      ├─ 恢复 SPSR_EL1 = 0 (EL0t)
      └─ eret             ← 切换到 EL0！
      │
      ▼
  用户程序 _start 开始执行 (EL0)
```

---

## 5.6 用户程序 — init.S

最简单的用户程序，嵌入内核二进制中：

```asm
/* userspace/init.S */
    .section ".text", "ax"
    .global _start
_start:
    /* write(1, msg, 23) */
    mov     x0, #1              /* fd = stdout */
    adr     x1, msg             /* buf = 消息地址 */
    mov     x2, #23             /* count = 23 */
    mov     x8, #64             /* syscall number = write */
    svc     #0                  /* 陷入内核！ */

    /* exit(0) */
    mov     x0, #0              /* status = 0 */
    mov     x8, #93             /* syscall number = exit */
    svc     #0                  /* 陷入内核，不会返回 */

    b       .                   /* 安全兜底 */

msg:
    .ascii  "Hello from user space!\n"
```

### 嵌入内核

```asm
/* userspace/init_blob.S */
    .section ".rodata", "a"
    .global _user_init_start
_user_init_start:
    .incbin "userspace/init.elf"    /* 嵌入编译好的 ELF */
_user_init_end:

    .global _user_init_size
_user_init_size:
    .quad   _user_init_end - _user_init_start
```

用户 ELF 链接到 `0x00400000`（标准用户程序基地址）：

```ld
/* userspace/user.ld */
ENTRY(_start)
SECTIONS {
    . = 0x00400000;
    .text : { *(.text .text.*) }
    .rodata : { *(.rodata .rodata.*) }
    .data : { *(.data .data.*) }
    .bss : { *(.bss .bss.*) }
}
```

---

## 5.7 TTBR0 切换 — 用户/内核页表隔离

上下文切换时需要切换 TTBR0（用户页表基址）：

```c
static void context_switch(struct task_struct *prev,
                           struct task_struct *next)
{
    /* 如果切换到有用户地址空间的进程 */
    if (next->mm) {
        /* 加载新的 TTBR0 */
        __asm__ volatile(
            "msr ttbr0_el1, %0\n"
            "isb\n"
            "tlbi vmalle1\n"
            "dsb ish\n"
            "isb\n"
            :: "r"(next->mm->pgd));
    }

    cpu_switch_to(prev, next);
}
```

---

## 5.8 完整执行流程

```
  start_kernel()
      │
      ├─ create_user_pgd()           分配用户页表
      ├─ load_elf_binary()           加载 init.elf 到用户空间
      ├─ map_user_page() × N         映射代码页 + 栈页
      ├─ create_user_task()          创建进程控制块
      │
      ├─ schedule()                  让出 CPU
      │      │
      │      └─ cpu_switch_to(idle, init)
      │              │
      │              └─ ret_to_user → kernel_exit 0 → eret
      │                      │
      │                      ▼
      │              ┌──────────────┐
      │              │  init _start │  (EL0)
      │              │  SVC #0      │  write("Hello...")
      │              └──────┬───────┘
      │                     │
      │              el0_svc → sys_write → UART 输出
      │              kernel_exit 0 → eret → 回到 EL0
      │                     │
      │              ┌──────┴───────┐
      │              │  SVC #0      │  exit(0)
      │              └──────┬───────┘
      │                     │
      │              sys_exit → TASK_DEAD → schedule()
      │                     │
      └─── idle 继续运行 ◄──┘
```

---

## 5.9 Phase 5 核心概念总结

| 概念 | 说明 |
|------|------|
| **SVC #0** | 用户态陷入内核的指令 |
| **ESR_EL1.EC** | 异常原因编码，0x15 = SVC |
| **系统调用表** | 函数指针数组，按 x8 索引 |
| **ELF 加载** | 解析 PT_LOAD 段，分配物理页，映射到用户虚拟地址 |
| **TTBR0** | 用户页表基址，每次切换进程时更新 |
| **ret_to_user** | 新进程首次 eret 到 EL0 的入口 |
| **SP_EL0** | 用户态栈指针，kernel_entry/exit 保存恢复 |

**Phase 5 奠定的基础**：有了系统调用和 ELF 加载，后续 Phase 可以让用户程序使用 VFS、网络等内核服务。
