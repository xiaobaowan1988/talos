/* SPDX-License-Identifier: GPL-2.0 */
/*
 * arm64os/userspace/init_blob.S
 *
 * 将用户态 init 程序的 ELF 二进制嵌入内核 .rodata 段。
 *
 * 构建流程：
 *   1. init.S → init.o（交叉编译器汇编）
 *   2. init.o → init.elf（链接，基址 0x00400000）
 *   3. 本文件通过 .incbin 将 init.elf 嵌入内核
 *
 * 内核通过 _user_init_start / _user_init_size 访问 ELF 数据。
 */

    .section ".rodata", "a"
    .align  3                   /* 8 字节对齐 */

    .global _user_init_start
    .global _user_init_size

_user_init_start:
    .incbin "userspace/init.elf"
_user_init_end:

_user_init_size:
    .quad   _user_init_end - _user_init_start
