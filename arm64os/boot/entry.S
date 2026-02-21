/*
 * entry.S — ARM64 kernel entry point
 *
 * This is the very first code that runs after QEMU loads the kernel.
 * Corresponds to: arch/arm64/kernel/pi/relocate.c, setup.c, head.S
 *
 * Execution flow:
 *   QEMU reset → _start (EL2 or EL1)
 *     → drop to EL1 if needed
 *     → set up stack, exception vectors
 *     → zero BSS
 *     → call kernel_main()
 *
 * ARM64 Exception Levels:
 *   EL3 — Secure monitor (TrustZone, not used here)
 *   EL2 — Hypervisor    (QEMU starts us here on virt machine)
 *   EL1 — Kernel        (where we want to run)
 *   EL0 — User space
 */

.section ".text"
.global _start

/* ─── kernel entry point ──────────────────────────────────────────────── */
_start:
    /*
     * Step 1: Read CurrentEL — bits[3:2] encode the exception level.
     *         EL2 = 0b1000 = 8,  EL1 = 0b0100 = 4
     */
    mrs     x0, CurrentEL
    lsr     x0, x0, #2
    cmp     x0, #2
    b.eq    _el2_to_el1         /* running at EL2 → drop to EL1 */
    b       _el1_entry           /* already at EL1 */

/* ─── drop from EL2 to EL1 ───────────────────────────────────────────── */
_el2_to_el1:
    /*
     * HCR_EL2.RW = 1 → EL1 executes as AArch64 (not AArch32)
     *            .E2H = 0 → standard EL2 (no VHE)
     * See: arch/arm64/kernel/head.S el2_setup
     */
    mov     x0, #(1 << 31)          /* RW bit */
    msr     hcr_el2, x0
    isb

    /*
     * SCTLR_EL1 = 0 → MMU off, caches off, alignment check off.
     * Safe reset value before we set up page tables.
     */
    msr     sctlr_el1, xzr
    isb

    /*
     * Pre-load the EL1 stack pointer into SP_EL1 before eret.
     * After eret we land at EL1 and sp == _stack_top immediately.
     */
    adr     x0, _stack_top
    msr     sp_el1, x0

    /*
     * SPSR_EL2 — saved program state for eret:
     *   bits[3:0] = 0b0101 = EL1h  (use SP_EL1)
     *   bits[9:6] = 0b1111         (mask D/A/I/F interrupts)
     *   → 0x3c5
     */
    mov     x0, #0x3c5
    msr     spsr_el2, x0

    /* ELR_EL2 — where eret jumps to */
    adr     x0, _el1_entry
    msr     elr_el2, x0

    eret                            /* exception return → EL1, _el1_entry */

/* ─── EL1 entry ──────────────────────────────────────────────────────── */
_el1_entry:
    /* Set up stack (already set via SP_EL1 if we came from EL2) */
    adr     x0, _stack_top
    mov     sp, x0

    /*
     * Install exception vector table.
     * VBAR_EL1 must be 2KB-aligned (we align to 2KB in .balign below).
     * See: arch/arm64/kernel/traps.c + entry.S vectors
     */
    adr     x0, vectors
    msr     vbar_el1, x0
    isb

    /*
     * Zero the BSS section.
     * Linker script defines _bss_start and _bss_end.
     * See: arch/arm64/kernel/pi/relocate.c
     */
    adr     x0, _bss_start
    adr     x1, _bss_end
    cmp     x0, x1
    b.ge    _bss_done
_bss_loop:
    str     xzr, [x0], #8
    cmp     x0, x1
    b.lt    _bss_loop
_bss_done:

    /* Jump into C — should never return */
    bl      kernel_main

_halt:
    wfe
    b       _halt


/* ═══════════════════════════════════════════════════════════════════════
 * Exception Vector Table
 *
 * ARM64 requires 16 entries × 128 bytes each = 2048 bytes total,
 * aligned to 2KB. Four groups × four exception types:
 *
 *   Group 0: Current EL, SP_EL0  (rarely used)
 *   Group 1: Current EL, SP_ELx  (kernel exceptions)
 *   Group 2: Lower EL, AArch64   (syscalls from user space)
 *   Group 3: Lower EL, AArch32   (32-bit compat, ignored here)
 *
 * Each entry: Sync / IRQ / FIQ / SError
 *
 * See: arch/arm64/kernel/entry.S
 * ═══════════════════════════════════════════════════════════════════════ */

.balign 2048
vectors:

/* ── Group 0: Current EL, SP_EL0 ──────────────────────────────────────── */
.balign 128
    b       _exc_sync_el1       /* Sync   */
.balign 128
    b       _exc_irq_el1        /* IRQ    */
.balign 128
    b       _halt               /* FIQ    */
.balign 128
    b       _halt               /* SError */

/* ── Group 1: Current EL, SP_ELx ──────────────────────────────────────── */
.balign 128
    b       _exc_sync_el1       /* Sync   */
.balign 128
    b       _exc_irq_el1        /* IRQ    */
.balign 128
    b       _halt               /* FIQ    */
.balign 128
    b       _halt               /* SError */

/* ── Group 2: Lower EL, AArch64 ───────────────────────────────────────── */
.balign 128
    b       _exc_sync_el0       /* Sync  (syscall SVC #0 lands here) */
.balign 128
    b       _halt               /* IRQ   */
.balign 128
    b       _halt               /* FIQ   */
.balign 128
    b       _halt               /* SError */

/* ── Group 3: Lower EL, AArch32 ───────────────────────────────────────── */
.balign 128
    b       _halt
.balign 128
    b       _halt
.balign 128
    b       _halt
.balign 128
    b       _halt


/* ═══════════════════════════════════════════════════════════════════════
 * Exception handlers (minimal stubs for Phase 1)
 *
 * Save all registers → call C handler → restore → eret
 * See: arch/arm64/kernel/entry-common.c, traps.c
 * ═══════════════════════════════════════════════════════════════════════ */

/*
 * SAVE_ALL / RESTORE_ALL — push/pop the full register frame.
 * We allocate 34×8 = 272 bytes: x0-x29, lr, sp_el0, elr_el1, spsr_el1.
 * This mirrors struct pt_regs in arch/arm64/include/asm/ptrace.h.
 */
.macro SAVE_ALL
    sub     sp, sp, #(34 * 8)
    stp     x0,  x1,  [sp, #0]
    stp     x2,  x3,  [sp, #16]
    stp     x4,  x5,  [sp, #32]
    stp     x6,  x7,  [sp, #48]
    stp     x8,  x9,  [sp, #64]
    stp     x10, x11, [sp, #80]
    stp     x12, x13, [sp, #96]
    stp     x14, x15, [sp, #112]
    stp     x16, x17, [sp, #128]
    stp     x18, x19, [sp, #144]
    stp     x20, x21, [sp, #160]
    stp     x22, x23, [sp, #176]
    stp     x24, x25, [sp, #192]
    stp     x26, x27, [sp, #208]
    stp     x28, x29, [sp, #224]
    mrs     x21, elr_el1
    mrs     x22, spsr_el1
    stp     x30, xzr, [sp, #240]    /* lr, pad */
    stp     x21, x22, [sp, #256]    /* elr, spsr */
.endm

.macro RESTORE_ALL
    ldp     x21, x22, [sp, #256]
    msr     elr_el1,  x21
    msr     spsr_el1, x22
    ldp     x0,  x1,  [sp, #0]
    ldp     x2,  x3,  [sp, #16]
    ldp     x4,  x5,  [sp, #32]
    ldp     x6,  x7,  [sp, #48]
    ldp     x8,  x9,  [sp, #64]
    ldp     x10, x11, [sp, #80]
    ldp     x12, x13, [sp, #96]
    ldp     x14, x15, [sp, #112]
    ldp     x16, x17, [sp, #128]
    ldp     x18, x19, [sp, #144]
    ldp     x20, x21, [sp, #160]
    ldp     x22, x23, [sp, #176]
    ldp     x24, x25, [sp, #192]
    ldp     x26, x27, [sp, #208]
    ldp     x28, x29, [sp, #224]
    ldp     x30, xzr, [sp, #240]
    add     sp, sp, #(34 * 8)
.endm

/* EL1 synchronous exception (e.g. data abort, instruction abort) */
_exc_sync_el1:
    SAVE_ALL
    mov     x0, sp                  /* arg0: pt_regs pointer */
    mrs     x1, esr_el1             /* arg1: exception syndrome */
    mrs     x2, far_el1             /* arg2: fault address */
    bl      exc_sync_el1_handler
    RESTORE_ALL
    eret

/* EL1 IRQ */
_exc_irq_el1:
    SAVE_ALL
    mov     x0, sp
    bl      exc_irq_el1_handler
    RESTORE_ALL
    eret

/* EL0 synchronous (syscall / user fault) */
_exc_sync_el0:
    SAVE_ALL
    mov     x0, sp
    mrs     x1, esr_el1
    bl      exc_sync_el0_handler
    RESTORE_ALL
    eret
