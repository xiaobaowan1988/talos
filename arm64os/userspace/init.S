/* SPDX-License-Identifier: GPL-2.0 */
/*
 * arm64os/userspace/init.S
 *
 * 最小用户态 init 进程
 *
 * 运行在 EL0（用户态），通过 SVC #0 发起系统调用。
 *
 * ARM64 系统调用约定：
 *   x8 = 系统调用号
 *   x0-x5 = 参数
 *   SVC #0 陷入内核
 *   返回值在 x0
 *
 * 系统调用号（ARM64 Linux ABI）：
 *   __NR_write = 64
 *   __NR_exit  = 93
 *
 * 本程序功能：
 *   1. write(1, "Hello from user space!\n", 23)
 *   2. exit(0)
 */

    .text
    .global _start
    .align  2

_start:
    /* ---- write(1, msg, 23) ---- */
    mov     x0, #1              /* fd = 1 (stdout) */
    adr     x1, msg             /* buf = message address (PC-relative) */
    mov     x2, #23             /* count = strlen("Hello from user space!\n") */
    mov     x8, #64             /* __NR_write = 64 */
    svc     #0                  /* syscall */

    /* ---- exit(0) ---- */
    mov     x0, #0              /* status = 0 */
    mov     x8, #93             /* __NR_exit = 93 */
    svc     #0                  /* syscall (does not return) */

    /* 安全兜底（不应到达） */
    b       .

    .align  2
msg:
    .ascii  "Hello from user space!\n"
    /* 不需要 NUL 终止（write 使用 count 参数）*/
