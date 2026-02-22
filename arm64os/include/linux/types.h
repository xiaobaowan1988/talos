/* SPDX-License-Identifier: GPL-2.0 */
/*
 * arm64os/include/linux/types.h
 *
 * 基础类型定义（freestanding 环境，不依赖 libc）
 * 参考：include/linux/types.h, include/uapi/linux/types.h
 */

#ifndef __LINUX_TYPES_H
#define __LINUX_TYPES_H

/* 无符号整数类型 */
typedef unsigned char           u8;
typedef unsigned short          u16;
typedef unsigned int            u32;
typedef unsigned long           u64;

/* 有符号整数类型 */
typedef signed char             s8;
typedef signed short            s16;
typedef signed int              s32;
typedef signed long             s64;

/* 物理/虚拟地址类型 */
typedef unsigned long           phys_addr_t;
typedef unsigned long           uintptr_t;

/* 标准 C 类型别名（与 stdint.h 兼容）*/
typedef u8   uint8_t;
typedef u16  uint16_t;
typedef u32  uint32_t;
typedef u64  uint64_t;

typedef s8   int8_t;
typedef s16  int16_t;
typedef s32  int32_t;
typedef s64  int64_t;

/* 大小类型 */
typedef unsigned long   size_t;
typedef signed long     ssize_t;

/* 空指针 */
#ifndef NULL
#define NULL ((void *)0)
#endif

/* 布尔类型 */
typedef int bool;
#define true  1
#define false 0

/* 内联 / const / volatile 辅助宏 */
#define __iomem     volatile        /* MMIO 内存访问标记 */

#endif /* __LINUX_TYPES_H */
