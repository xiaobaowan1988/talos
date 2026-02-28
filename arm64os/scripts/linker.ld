/*
 * arm64os/scripts/linker.ld
 *
 * Phase 1 链接脚本 — 物理地址模式（无MMU）
 *
 * QEMU virt machine 将 ARM64 Linux 格式镜像加载到物理地址 0x40080000。
 * Phase 1 直接在物理地址运行，不启用 MMU。
 * Phase 2 将切换到高虚拟地址 0xFFFF000040080000（启用 TTBR1 映射后）。
 *
 * 参考：arch/arm64/kernel/vmlinux.lds.S
 */

OUTPUT_ARCH(aarch64)
ENTRY(_start)

SECTIONS {
    /*
     * QEMU virt machine 将内核镜像加载到此物理地址。
     * ARM64 Boot Protocol 要求 2MB 对齐，0x40080000 = 0x40000000 + 0x80000（512KB 偏移）。
     * 实际上 QEMU 对 -kernel ELF 直接加载到 ELF load address，不要求 2MB 对齐。
     */
    . = 0x40080000;

    /*
     * .head.text：ARM64 Image 头部，head.S 必须在最前面。
     * KEEP 确保链接器不优化掉该节。
     */
    .head.text : {
        _text = .;
        KEEP(*(.head.text))
    }

    /* 代码段 */
    .text : {
        *(.text .text.*)
    }

    /* 只读数据段，4KB 对齐 */
    . = ALIGN(4096);
    .rodata : {
        *(.rodata .rodata.*)
    }

    /* 可读写数据段，4KB 对齐 */
    . = ALIGN(4096);
    .data : {
        _data = .;
        *(.data .data.*)
    }

    /*
     * BSS 段（未初始化数据），标记为 NOLOAD（不占 ELF 文件体积）。
     * head.S 在启动时负责将 [_bss_start, _bss_end) 清零。
     * 使用 16 字节对齐以配合 head.S 中 stp xzr, xzr 的步进方式。
     */
    . = ALIGN(16);
    _bss_start = .;
    .bss (NOLOAD) : {
        *(.bss .bss.*)
        *(COMMON)
    }
    . = ALIGN(16);
    _bss_end = .;

    _end = .;
}
