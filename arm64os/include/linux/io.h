/* SPDX-License-Identifier: GPL-2.0 */
/*
 * arm64os/include/linux/io.h
 *
 * MMIO 寄存器访问辅助函数 + 内存屏障
 *
 * 参考：include/asm-generic/io.h, arch/arm64/include/asm/barrier.h
 *
 * ARM64 Device-nGnRnE/nGnRE 内存属性已保证单设备 MMIO 顺序性，
 * 但跨设备或 DMA 场景仍需显式屏障。
 */

#ifndef __LINUX_IO_H
#define __LINUX_IO_H

#include <linux/types.h>

/*
 * MMIO 读写函数
 * volatile 确保编译器不优化掉硬件寄存器访问。
 */
static inline u32 readl(volatile void *addr)
{
    return *(volatile u32 *)addr;
}

static inline void writel(u32 val, volatile void *addr)
{
    *(volatile u32 *)addr = val;
}

static inline u64 readq(volatile void *addr)
{
    return *(volatile u64 *)addr;
}

static inline void writeq(u64 val, volatile void *addr)
{
    *(volatile u64 *)addr = val;
}

static inline u8 readb(volatile void *addr)
{
    return *(volatile u8 *)addr;
}

static inline void writeb(u8 val, volatile void *addr)
{
    *(volatile u8 *)addr = val;
}

/*
 * 内存屏障
 *
 * ARM64 使用 DMB (Data Memory Barrier) 指令：
 *   dmb ish    — 全序屏障（Inner Shareable 域）
 *   dmb ishst  — 写屏障（仅序列化 store 操作）
 *   dmb ishld  — 读屏障（仅序列化 load 操作）
 *
 * 参考：arch/arm64/include/asm/barrier.h
 */
#define mb()    __asm__ volatile("dmb ish"   ::: "memory")
#define wmb()   __asm__ volatile("dmb ishst" ::: "memory")
#define rmb()   __asm__ volatile("dmb ishld" ::: "memory")

/* cpu_relax: 自旋等待时让出流水线资源 */
#define cpu_relax() __asm__ volatile("yield" ::: "memory")

#endif /* __LINUX_IO_H */
