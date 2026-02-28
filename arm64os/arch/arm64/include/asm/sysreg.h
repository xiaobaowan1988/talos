/* SPDX-License-Identifier: GPL-2.0 */
/*
 * arm64os/arch/arm64/include/asm/sysreg.h
 *
 * ARMv8-A 系统寄存器定义
 * 参考：arch/arm64/include/asm/sysreg.h
 *       ARMv8-A Architecture Reference Manual (ARM DDI 0487)
 */

#ifndef __ASM_SYSREG_H
#define __ASM_SYSREG_H

/*
 * ============================================================
 * SCTLR_EL1: System Control Register (EL1)
 * 参考：ARMv8-A ARM, Section D13.2.113
 * ============================================================
 */
#define SCTLR_EL1_M        (1UL << 0)   /* MMU 使能 */
#define SCTLR_EL1_A        (1UL << 1)   /* 对齐检查使能 */
#define SCTLR_EL1_C        (1UL << 2)   /* Data Cache 使能 */
#define SCTLR_EL1_SA       (1UL << 3)   /* EL1 栈对齐检查 */
#define SCTLR_EL1_SA0      (1UL << 4)   /* EL0 栈对齐检查 */
#define SCTLR_EL1_I        (1UL << 12)  /* Instruction Cache 使能 */
#define SCTLR_EL1_WXN      (1UL << 19)  /* Write XOR Execute（可写则不可执行）*/
#define SCTLR_EL1_EE       (1UL << 25)  /* 异常字节序（0=小端 LE）*/
#define SCTLR_EL1_UCI      (1UL << 26)  /* 允许 EL0 执行 cache 维护指令 */

/* Phase 1 使用：MMU 关闭时的安全初始值（只打开指令 cache，关闭 MMU/Dcache） */
#define SCTLR_EL1_RESET    0UL

/*
 * ============================================================
 * HCR_EL2: Hypervisor Configuration Register
 * 参考：ARMv8-A ARM, Section D13.2.48
 * ============================================================
 */
#define HCR_EL2_VM         (1UL << 0)   /* 虚拟化使能 */
#define HCR_EL2_HCD        (1UL << 29)  /* 禁用 HVC 指令 */
#define HCR_EL2_RW         (1UL << 31)  /* EL1 使用 AArch64（必须置1）*/

/*
 * ============================================================
 * SPSR（Saved Program Status Register）EL 返回值
 * ============================================================
 */
/* EL2 → EL1h 的 SPSR 值: M[4:0]=EL1h(0b00101), DAIF=1111 */
#define SPSR_EL1H_DAIF_MASKED   0x3c5UL

/*
 * ============================================================
 * TCR_EL1: Translation Control Register
 * 参考：ARMv8-A ARM, Section D13.2.131
 * Phase 2 将使用这些定义配置页表
 * ============================================================
 */
#define TCR_T0SZ(x)        ((unsigned long)(x) << 0)
#define TCR_T1SZ(x)        ((unsigned long)(x) << 16)

/* 页粒度：TTBR0 (TG0) */
#define TCR_TG0_4K         (0UL << 14)
#define TCR_TG0_64K        (1UL << 14)
#define TCR_TG0_16K        (2UL << 14)

/* 页粒度：TTBR1 (TG1) */
#define TCR_TG1_16K        (1UL << 30)
#define TCR_TG1_4K         (2UL << 30)
#define TCR_TG1_64K        (3UL << 30)

/* 内存属性：Inner/Outer cacheability */
#define TCR_IRGN0_NC       (0UL << 8)   /* Inner non-cacheable */
#define TCR_IRGN0_WBWA     (1UL << 8)   /* Inner write-back, write-allocate */
#define TCR_ORGN0_NC       (0UL << 10)  /* Outer non-cacheable */
#define TCR_ORGN0_WBWA     (1UL << 10)  /* Outer write-back, write-allocate */

#define TCR_IRGN1_NC       (0UL << 24)
#define TCR_IRGN1_WBWA     (1UL << 24)
#define TCR_ORGN1_NC       (0UL << 26)
#define TCR_ORGN1_WBWA     (1UL << 26)

/* 共享属性 */
#define TCR_SH0_NONE       (0UL << 12)
#define TCR_SH0_OUTER      (2UL << 12)
#define TCR_SH0_INNER      (3UL << 12)

#define TCR_SH1_NONE       (0UL << 28)
#define TCR_SH1_INNER      (3UL << 28)

/* 物理地址空间大小 (IPS) */
#define TCR_IPS_32BIT      (0UL << 32)  /* 4GB */
#define TCR_IPS_36BIT      (1UL << 32)  /* 64GB */
#define TCR_IPS_40BIT      (2UL << 32)  /* 1TB */
#define TCR_IPS_42BIT      (3UL << 32)  /* 4TB */
#define TCR_IPS_44BIT      (4UL << 32)  /* 16TB */
#define TCR_IPS_48BIT      (5UL << 32)  /* 256TB */

/* 其他 TCR 选项 */
#define TCR_ASID16         (1UL << 36)  /* 16-bit ASID */
#define TCR_TBI0           (1UL << 37)  /* Top Byte Ignore (TTBR0) */
#define TCR_TBI1           (1UL << 38)  /* Top Byte Ignore (TTBR1) */

/*
 * ============================================================
 * MAIR_EL1: Memory Attribute Indirection Register
 * 定义内存属性索引 0-7 对应的具体属性
 * ============================================================
 */
#define MAIR_ATTR_DEVICE_nGnRnE  0x00UL  /* Device: no Gather/Reorder/Early-write-ack */
#define MAIR_ATTR_DEVICE_nGnRE   0x04UL  /* Device: no Gather/Reorder */
#define MAIR_ATTR_NORMAL_NC      0x44UL  /* Normal, Non-cacheable */
#define MAIR_ATTR_NORMAL         0xffUL  /* Normal, write-back, read/write-allocate */

#define MAIR_ATTR(idx, attr)     ((attr) << ((idx) * 8))

/* 常用内存属性索引（与页表项 AttrIdx 字段对应）*/
#define MT_DEVICE_nGnRnE   0
#define MT_DEVICE_nGnRE    1
#define MT_NORMAL_NC       2
#define MT_NORMAL          3

/*
 * ============================================================
 * DAIF: 中断屏蔽位
 * ============================================================
 */
#define DAIF_DBG_BIT       (1 << 3)  /* Debug 异常屏蔽 */
#define DAIF_ABT_BIT       (1 << 2)  /* SError (异步中止) 屏蔽 */
#define DAIF_IRQ_BIT       (1 << 1)  /* IRQ 屏蔽 */
#define DAIF_FIQ_BIT       (1 << 0)  /* FIQ 屏蔽 */

/*
 * ============================================================
 * CurrentEL: 当前异常级别
 * ============================================================
 */
#define CurrentEL_EL0      (0 << 2)
#define CurrentEL_EL1      (1 << 2)
#define CurrentEL_EL2      (2 << 2)
#define CurrentEL_EL3      (3 << 2)
#define CurrentEL_MASK     (3 << 2)

#endif /* __ASM_SYSREG_H */
