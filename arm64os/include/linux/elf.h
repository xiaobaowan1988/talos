/* SPDX-License-Identifier: GPL-2.0 */
/*
 * arm64os/include/linux/elf.h
 *
 * ELF64 文件格式定义（AArch64）
 *
 * 参考：include/uapi/linux/elf.h
 *       arch/arm64/include/asm/elf.h
 *
 * Phase 5 使用：ELF 加载器（fs/binfmt_elf.c）解析用户态可执行文件。
 * 仅定义 ELF64 格式（ARM64 无需 ELF32）。
 */

#ifndef __LINUX_ELF_H
#define __LINUX_ELF_H

#include <linux/types.h>

/*
 * ============================================================
 * ELF 魔数
 * ============================================================
 */
#define ELFMAG0     0x7f
#define ELFMAG1     'E'
#define ELFMAG2     'L'
#define ELFMAG3     'F'
#define SELFMAG     4       /* sizeof(ELFMAG) */

/*
 * e_ident[] 索引
 */
#define EI_MAG0     0       /* 魔数字节 0 */
#define EI_MAG1     1       /* 魔数字节 1 */
#define EI_MAG2     2       /* 魔数字节 2 */
#define EI_MAG3     3       /* 魔数字节 3 */
#define EI_CLASS    4       /* 文件类别（32/64位）*/
#define EI_DATA     5       /* 数据编码（大/小端）*/
#define EI_VERSION  6       /* ELF 版本 */
#define EI_NIDENT   16      /* e_ident 数组大小 */

/* EI_CLASS 值 */
#define ELFCLASS64  2       /* 64位 ELF */

/* EI_DATA 值 */
#define ELFDATA2LSB 1       /* 小端（Little-Endian）*/

/*
 * ============================================================
 * ELF 头部类型（e_type）
 * ============================================================
 */
#define ET_EXEC     2       /* 可执行文件 */
#define ET_DYN      3       /* 共享目标文件 / PIE */

/*
 * ============================================================
 * 机器架构（e_machine）
 * ============================================================
 */
#define EM_AARCH64  183     /* ARM AARCH64 */

/*
 * ============================================================
 * Program Header 类型（p_type）
 * ============================================================
 */
#define PT_NULL     0       /* 未使用 */
#define PT_LOAD     1       /* 可加载段 */
#define PT_DYNAMIC  2       /* 动态链接信息 */
#define PT_INTERP   3       /* 解释器路径 */
#define PT_NOTE     4       /* 附加信息 */
#define PT_PHDR     6       /* Program Header 表自身 */

/*
 * ============================================================
 * Program Header 标志（p_flags）
 * ============================================================
 */
#define PF_X        (1 << 0)    /* 可执行 */
#define PF_W        (1 << 1)    /* 可写 */
#define PF_R        (1 << 2)    /* 可读 */

/*
 * ============================================================
 * ELF64 头部（64 字节）
 *
 * 参考：include/uapi/linux/elf.h Elf64_Ehdr
 * ============================================================
 */
typedef struct {
    unsigned char   e_ident[EI_NIDENT]; /* 魔数 + 类别/编码/版本 */
    uint16_t        e_type;             /* 文件类型（ET_EXEC/ET_DYN）*/
    uint16_t        e_machine;          /* 目标架构（EM_AARCH64=183）*/
    uint32_t        e_version;          /* ELF 版本 */
    uint64_t        e_entry;            /* 程序入口虚拟地址 */
    uint64_t        e_phoff;            /* Program Header 表文件偏移 */
    uint64_t        e_shoff;            /* Section Header 表文件偏移 */
    uint32_t        e_flags;            /* 处理器特定标志 */
    uint16_t        e_ehsize;           /* ELF 头大小（=64）*/
    uint16_t        e_phentsize;        /* 单个 Program Header 大小（=56）*/
    uint16_t        e_phnum;            /* Program Header 数量 */
    uint16_t        e_shentsize;        /* 单个 Section Header 大小 */
    uint16_t        e_shnum;            /* Section Header 数量 */
    uint16_t        e_shstrndx;         /* 字符串表 Section 索引 */
} Elf64_Ehdr;

/*
 * ============================================================
 * ELF64 Program Header（56 字节）
 *
 * 描述一个可加载的段（segment），告诉加载器如何将
 * 文件中的内容映射到进程地址空间。
 *
 * 参考：include/uapi/linux/elf.h Elf64_Phdr
 * ============================================================
 */
typedef struct {
    uint32_t        p_type;     /* 段类型（PT_LOAD, PT_DYNAMIC 等）*/
    uint32_t        p_flags;    /* 段权限（PF_R | PF_W | PF_X）*/
    uint64_t        p_offset;   /* 段在文件中的偏移 */
    uint64_t        p_vaddr;    /* 段的虚拟地址 */
    uint64_t        p_paddr;    /* 段的物理地址（通常同 vaddr）*/
    uint64_t        p_filesz;   /* 段在文件中的大小 */
    uint64_t        p_memsz;    /* 段在内存中的大小（>= filesz）*/
    uint64_t        p_align;    /* 段对齐要求 */
} Elf64_Phdr;

#endif /* __LINUX_ELF_H */
