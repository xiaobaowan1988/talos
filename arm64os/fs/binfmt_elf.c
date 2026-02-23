/* SPDX-License-Identifier: GPL-2.0 */
/*
 * arm64os/fs/binfmt_elf.c
 *
 * ELF64 加载器
 *
 * 参考：fs/binfmt_elf.c
 *
 * Phase 5 实现：
 *   - load_elf_binary()：从内存中的 ELF 映像解析头部和段，
 *     将 PT_LOAD 段拷贝到用户空间页面并建立页表映射。
 *   - 不依赖文件系统（ELF 数据以指针+长度传入）。
 *   - 仅支持静态链接的 ELF64 AArch64 可执行文件。
 */

#include <linux/types.h>
#include <linux/elf.h>
#include <asm/memory.h>
#include <asm/pgtable.h>

/* 外部函数 */
void boot_printk(const char *s);
void boot_printk_hex(unsigned long val);

/* Buddy 分配器 */
struct page;
struct page *alloc_pages(unsigned int order);
void *page_address(struct page *page);

/* 用户页表操作（mmu.c） */
int map_user_page(unsigned long pgd_phys, unsigned long va,
                  unsigned long pa, unsigned long attrs);

/*
 * ============================================================
 * memcpy / memset 辅助函数（freestanding 环境）
 * ============================================================
 */
static void *elf_memcpy(void *dst, const void *src, size_t n)
{
    unsigned char *d = dst;
    const unsigned char *s = src;
    while (n--)
        *d++ = *s++;
    return dst;
}

static void *elf_memset(void *s, int c, size_t n)
{
    unsigned char *p = s;
    while (n--)
        *p++ = (unsigned char)c;
    return s;
}

static int elf_memcmp(const void *a, const void *b, size_t n)
{
    const unsigned char *pa = a, *pb = b;
    while (n--) {
        if (*pa != *pb)
            return *pa - *pb;
        pa++; pb++;
    }
    return 0;
}

/*
 * ============================================================
 * load_elf_binary - 加载 ELF 映像到用户地址空间
 *
 * @elf_data:  ELF 文件数据起始地址（内核虚拟地址）
 * @elf_size:  ELF 文件大小
 * @pgd_phys:  用户进程的 PGD 物理地址
 * @entry_out: 输出：程序入口虚拟地址
 *
 * 返回 0 成功，负数失败。
 *
 * 流程：
 *   1. 验证 ELF 头部（魔数、类别、架构）
 *   2. 遍历 Program Header，对每个 PT_LOAD 段：
 *      a. 分配物理页
 *      b. 拷贝文件内容（p_filesz 字节）
 *      c. BSS 区域清零（p_memsz - p_filesz）
 *      d. 在用户页表中映射（根据 p_flags 设置权限）
 *   3. 返回入口点地址
 *
 * 参考：fs/binfmt_elf.c load_elf_binary()
 * ============================================================
 */
int load_elf_binary(const void *elf_data, size_t elf_size,
                    unsigned long pgd_phys, unsigned long *entry_out)
{
    const Elf64_Ehdr *ehdr = (const Elf64_Ehdr *)elf_data;
    const Elf64_Phdr *phdr;
    const unsigned char elf_magic[] = { ELFMAG0, ELFMAG1, ELFMAG2, ELFMAG3 };
    int i;

    /* Step 1: 验证 ELF 头部 */
    if (elf_size < sizeof(Elf64_Ehdr)) {
        boot_printk("[elf] ERROR: file too small for ELF header\n");
        return -1;
    }

    if (elf_memcmp(ehdr->e_ident, elf_magic, SELFMAG) != 0) {
        boot_printk("[elf] ERROR: bad ELF magic\n");
        return -1;
    }

    if (ehdr->e_ident[EI_CLASS] != ELFCLASS64) {
        boot_printk("[elf] ERROR: not ELF64\n");
        return -1;
    }

    if (ehdr->e_ident[EI_DATA] != ELFDATA2LSB) {
        boot_printk("[elf] ERROR: not little-endian\n");
        return -1;
    }

    if (ehdr->e_machine != EM_AARCH64) {
        boot_printk("[elf] ERROR: not AArch64 (e_machine=");
        boot_printk_hex(ehdr->e_machine);
        boot_printk(")\n");
        return -1;
    }

    if (ehdr->e_type != ET_EXEC && ehdr->e_type != ET_DYN) {
        boot_printk("[elf] ERROR: not executable (e_type=");
        boot_printk_hex(ehdr->e_type);
        boot_printk(")\n");
        return -1;
    }

    boot_printk("[elf] ELF header OK: entry=");
    boot_printk_hex(ehdr->e_entry);
    boot_printk(", phnum=");
    boot_printk_hex(ehdr->e_phnum);
    boot_printk("\n");

    /* Step 2: 遍历 Program Headers */
    phdr = (const Elf64_Phdr *)((const unsigned char *)elf_data + ehdr->e_phoff);

    for (i = 0; i < ehdr->e_phnum; i++) {
        const Elf64_Phdr *ph = &phdr[i];
        unsigned long seg_va, seg_va_end;
        unsigned long page_va, page_pa;
        unsigned long attrs;
        size_t file_off, file_remain;
        struct page *pg;

        if (ph->p_type != PT_LOAD)
            continue;

        if (ph->p_memsz == 0)
            continue;

        /* 确定页属性 */
        if (ph->p_flags & PF_X)
            attrs = PD_USER_EXEC;
        else
            attrs = PD_USER_DATA;

        seg_va = ph->p_vaddr & PAGE_MASK;
        seg_va_end = PAGE_ALIGN_UP(ph->p_vaddr + ph->p_memsz);

        boot_printk("[elf] PT_LOAD: va=");
        boot_printk_hex(ph->p_vaddr);
        boot_printk(" filesz=");
        boot_printk_hex(ph->p_filesz);
        boot_printk(" memsz=");
        boot_printk_hex(ph->p_memsz);
        boot_printk(" flags=");
        boot_printk_hex(ph->p_flags);
        boot_printk("\n");

        /* 逐页分配、拷贝、映射 */
        file_off = 0;
        file_remain = ph->p_filesz;

        for (page_va = seg_va; page_va < seg_va_end; page_va += PAGE_SIZE) {
            size_t copy_bytes = 0;
            size_t page_offset;

            /* 分配一个物理页 */
            pg = alloc_pages(0);
            if (!pg) {
                boot_printk("[elf] ERROR: out of memory for user page\n");
                return -1;
            }
            page_pa = (unsigned long)page_address(pg);

            /* 先清零整页 */
            elf_memset((void *)page_pa, 0, PAGE_SIZE);

            /*
             * 计算本页需要拷贝的文件内容。
             * p_vaddr 可能不是页对齐的，第一页需要偏移处理。
             */
            if (page_va < ph->p_vaddr) {
                /* 第一页：段起始地址可能不在页首 */
                page_offset = ph->p_vaddr - page_va;
            } else {
                page_offset = 0;
            }

            if (file_remain > 0) {
                copy_bytes = PAGE_SIZE - page_offset;
                if (copy_bytes > file_remain)
                    copy_bytes = file_remain;

                elf_memcpy((void *)(page_pa + page_offset),
                           (const unsigned char *)elf_data + ph->p_offset + file_off,
                           copy_bytes);

                file_off += copy_bytes;
                file_remain -= copy_bytes;
            }

            /* 映射到用户页表 */
            if (map_user_page(pgd_phys, page_va, page_pa, attrs) != 0) {
                boot_printk("[elf] ERROR: map_user_page failed for va=");
                boot_printk_hex(page_va);
                boot_printk("\n");
                return -1;
            }
        }
    }

    /* Step 3: 返回入口点 */
    *entry_out = ehdr->e_entry;
    return 0;
}
