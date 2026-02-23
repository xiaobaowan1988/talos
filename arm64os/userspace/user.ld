/*
 * arm64os/userspace/user.ld
 *
 * 用户程序链接脚本
 *
 * 用户代码加载到虚拟地址 0x00400000（标准 Linux 用户空间文本段起始地址）。
 * Phase 5 的 ELF 加载器根据 Program Header 中的 p_vaddr 进行映射。
 */

OUTPUT_ARCH(aarch64)
ENTRY(_start)

SECTIONS {
    /* 用户代码起始地址 */
    . = 0x00400000;

    .text : {
        *(.text .text.*)
    }

    .rodata : {
        *(.rodata .rodata.*)
    }

    .data : {
        *(.data .data.*)
    }

    .bss : {
        *(.bss .bss.*)
    }
}
