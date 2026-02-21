#ifndef _TYPES_H
#define _TYPES_H

typedef unsigned char      u8;
typedef unsigned short     u16;
typedef unsigned int       u32;
typedef unsigned long      u64;
typedef unsigned long      ulong;
typedef unsigned long      size_t;
typedef unsigned long      uintptr_t;
typedef long               ptrdiff_t;

#define NULL  ((void *)0)
#define true  1
#define false 0

#define ARRAY_SIZE(x)  (sizeof(x) / sizeof((x)[0]))
#define ALIGN_UP(x, a)   (((x) + (a) - 1) & ~((a) - 1))
#define ALIGN_DOWN(x, a) ((x) & ~((a) - 1))

#endif /* _TYPES_H */
