/*
 * start.S - ARM64 bare-metal entry point
 *
 * This is the very first code that runs when the CPU powers on.
 * QEMU's -machine virt loads our image at 0x40000000 and jumps here.
 *
 * What this code does:
 *   1. Set up the stack pointer (SP) so C code can use function calls and local variables
 *   2. Zero out the .bss section (uninitialized global variables must start as 0)
 *   3. Jump to the C kernel_main() function
 *   4. If kernel_main returns, halt the CPU in a loop
 */

.section .text.boot
.global _start

_start:
    /* ---------------------------------------------------------------
     * Step 1: Set up the stack
     *
     * ARM64 stack grows downward (from high address to low address).
     * We place the stack at _stack_top (defined in linker script).
     * The 'ldr' pseudo-instruction loads a 64-bit address.
     * ---------------------------------------------------------------
     */
    ldr     x0, =_stack_top
    mov     sp, x0

    /* ---------------------------------------------------------------
     * Step 2: Zero the .bss section
     *
     * The .bss section holds uninitialized global/static variables.
     * The C standard requires them to be zero at startup.
     * The linker script provides _bss_start and _bss_end symbols.
     * ---------------------------------------------------------------
     */
    ldr     x0, =_bss_start
    ldr     x1, =_bss_end
bss_clear:
    cmp     x0, x1
    b.ge    bss_done          /* if x0 >= x1, we are done */
    str     xzr, [x0], #8     /* store zero at [x0], then x0 += 8 */
    b       bss_clear
bss_done:

    /* ---------------------------------------------------------------
     * Step 3: Jump to C code
     *
     * 'bl' = Branch with Link: saves return address in LR (x30),
     * then jumps to kernel_main.
     * ---------------------------------------------------------------
     */
    bl      kernel_main

    /* ---------------------------------------------------------------
     * Step 4: Halt - if kernel_main returns, spin forever
     *
     * 'wfe' = Wait For Event: puts the CPU in low-power idle state.
     * The infinite loop ensures we never execute garbage memory.
     * ---------------------------------------------------------------
     */
halt:
    wfe
    b       halt
