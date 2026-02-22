/* SPDX-License-Identifier: GPL-2.0 */
/*
 * arm64os/arch/arm64/mm/tlb.S
 *
 * TLB（Translation Lookaside Buffer）操作辅助函数
 *
 * 参考：arch/arm64/include/asm/tlbflush.h
 *       arch/arm64/mm/tlb.c
 *       ARMv8-A ARM Section D5.10（TLB maintenance instructions）
 *
 * TLB 是地址翻译的硬件缓存。修改页表后必须无效化相关 TLB 条目，
 * 否则 CPU 会使用过时的翻译结果导致错误。
 *
 * ARMv8 TLB 无效化指令格式：
 *   TLBI <type><level>[IS] [, <Xt>]
 *
 *   level: E1=EL1, E2=EL2, E3=EL3
 *   IS:    Inner Shareable（多核广播无效化，单核可省略）
 *   type:
 *     VMALLE1  — 无效化当前VMID下EL1的所有TLB条目（忽略ASID）
 *     ALLE1    — 无效化EL1所有TLB条目
 *     VAE1     — 按虚拟地址无效化（Xt=VA>>12）
 *     ASIDE1   — 按ASID无效化
 */

    .section ".text", "ax"

/*
 * tlb_flush_all - 无效化所有 EL1 TLB 条目
 *
 * 使用场景：
 *   - MMU 开启前后
 *   - 整体替换页表时（如 context switch with new ASID）
 *
 * 注意：在 SMP 系统中使用 IS（Inner Shareable）变体以广播到所有核心。
 * Phase 2 单核实现，使用 vmalle1（非IS），足够。
 *
 * C 原型：void tlb_flush_all(void);
 */
    .global tlb_flush_all
tlb_flush_all:
    /*
     * TLBI VMALLE1：无效化当前 EL1 下所有 TLB 条目。
     * "VMALL" = Virtual Memory ALl（全虚拟内存），"E1" = Exception Level 1。
     * 不区分 ASID，适合全局页表替换场景。
     *
     * 参考：ARMv8-A ARM Section C5.5.36
     */
    tlbi    vmalle1

    /*
     * dsb ish：Data Synchronization Barrier（Inner Shareable）
     * 确保 TLBI 指令对所有共享内存的观察者（多核）可见，
     * 且之前的所有内存访问（包括页表更新）在此点完成。
     *
     * 在 TLB 无效化后必须有 DSB，保证后续页表遍历看到新状态。
     */
    dsb     ish

    /*
     * isb：Instruction Synchronization Barrier
     * 刷新流水线，确保后续指令重新进行地址翻译
     * （不能使用已被 TLBI 无效化的旧翻译结果）。
     */
    isb

    ret

/*
 * tlb_flush_page - 无效化单个虚拟地址的 TLB 条目
 *
 * 参数：
 *   x0 = 虚拟地址（任意对齐，自动按页对齐处理）
 *
 * 比 tlb_flush_all 更精细，修改单页映射时使用，性能更好。
 *
 * C 原型：void tlb_flush_page(unsigned long va);
 */
    .global tlb_flush_page
tlb_flush_page:
    /*
     * VAE1（Virtual Address by Exception Level 1）：
     * Xt = VA[63:12]（虚拟页号，右移12位去掉页内偏移）
     *
     * 参考：ARMv8-A ARM Section C5.5.32
     */
    lsr     x0, x0, #12            /* x0 = VA >> 12（虚拟页号）*/
    tlbi    vae1, x0               /* 无效化该虚拟页的TLB条目 */

    dsb     ish
    isb
    ret
