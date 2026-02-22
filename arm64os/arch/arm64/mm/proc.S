/* SPDX-License-Identifier: GPL-2.0 */
/*
 * arm64os/arch/arm64/mm/proc.S
 *
 * CPU MMU相关初始化例程
 *
 * 参考：arch/arm64/mm/proc.S
 *
 * 提供两个汇编函数：
 *
 *   cpu_init()      — 配置内存属性寄存器（MAIR_EL1）和地址翻译控制（TCR_EL1）
 *   enable_mmu(pgd) — 写入 TTBR0_EL1，开启 SCTLR_EL1.M（MMU使能位）
 *
 * 调用顺序（由 mmu_init() 在 C 中编排）：
 *   1. create_page_tables()  — 构建恒等映射页表（C函数，mmu.c）
 *   2. cpu_init()            — 设置 MAIR/TCR（本文件）
 *   3. enable_mmu(pgd_phys) — 开启MMU（本文件）
 */

#include <asm/sysreg.h>

/*
 * ============================================================
 * cpu_init - 初始化内存属性和地址翻译控制寄存器
 *
 * 必须在 enable_mmu 之前调用，否则 MMU 行为未定义。
 *
 * 配置：
 *   MAIR_EL1 — 内存属性索引表（4个条目）
 *   TCR_EL1  — 地址翻译控制（VA大小、粒度、缓存策略）
 *
 * C 原型：void cpu_init(void);
 * ============================================================
 */
    .section ".text", "ax"
    .global cpu_init
cpu_init:
    /*
     * MAIR_EL1 — Memory Attribute Indirection Register
     *
     * 每个属性占8位，共支持8个属性槽（index 0-7）。
     * 我们使用4个：
     *   index 0: Device-nGnRnE（MMIO设备内存，最严格顺序）
     *   index 1: Device-nGnRE （MMIO设备内存，无重排序）
     *   index 2: Normal Non-Cacheable（普通内存，不可缓存）
     *   index 3: Normal Write-Back（普通内存，写回缓存，性能最优）
     *
     * 参考：ARMv8-A ARM Section D13.2.95 MAIR_EL1
     */
    ldr     x0, =( \
        (0x00UL << (0 * 8)) |   /* idx 0: Device-nGnRnE */ \
        (0x04UL << (1 * 8)) |   /* idx 1: Device-nGnRE  */ \
        (0x44UL << (2 * 8)) |   /* idx 2: Normal-NC     */ \
        (0xFFUL << (3 * 8))  )  /* idx 3: Normal-WBWA   */
    msr     mair_el1, x0

    /*
     * TCR_EL1 — Translation Control Register
     *
     * 控制两个地址翻译范围：
     *   TTBR0：低地址空间（用户/恒等映射），T0SZ=16 → 48位VA
     *   TTBR1：高地址空间（内核），T1SZ=16 → 48位VA（Phase 2 暂不使用）
     *
     * 字段说明（参考 ARMv8-A ARM Section D13.2.131）：
     *   T0SZ[5:0]   = 16：TTBR0 VA 大小 = 64-16 = 48位
     *   IRGN0[9:8]  = 01：TTBR0 Inner Write-Back Read/Write-Allocate
     *   ORGN0[11:10]= 01：TTBR0 Outer Write-Back Read/Write-Allocate
     *   SH0[13:12]  = 11：TTBR0 Inner Shareable
     *   TG0[15:14]  = 00：TTBR0 Granule = 4KB
     *   T1SZ[21:16] = 16：TTBR1 VA 大小 = 48位
     *   IRGN1[25:24]= 01：TTBR1 Inner Write-Back
     *   ORGN1[27:26]= 01：TTBR1 Outer Write-Back
     *   SH1[29:28]  = 11：TTBR1 Inner Shareable
     *   TG1[31:30]  = 10：TTBR1 Granule = 4KB（TG1=10 对应4KB）
     *   IPS[34:32]  = 101：支持的物理地址空间 = 48位（256TB）
     *
     * QEMU Cortex-A72 支持 48位PA（ARMv8.0-A），所以 IPS=101。
     */
    ldr     x0, =( \
        (16UL  <<  0) |     /* T0SZ = 16（48位TTBR0 VA）*/ \
        (1UL   <<  8) |     /* IRGN0 = 01（Inner WB-RA）*/ \
        (1UL   << 10) |     /* ORGN0 = 01（Outer WB-RA）*/ \
        (3UL   << 12) |     /* SH0   = 11（Inner Shareable）*/ \
        (0UL   << 14) |     /* TG0   = 00（4KB granule）*/ \
        (16UL  << 16) |     /* T1SZ = 16（48位TTBR1 VA）*/ \
        (1UL   << 24) |     /* IRGN1 = 01 */ \
        (1UL   << 26) |     /* ORGN1 = 01 */ \
        (3UL   << 28) |     /* SH1   = 11 */ \
        (2UL   << 30) |     /* TG1   = 10（4KB granule for TTBR1）*/ \
        (5UL   << 32)  )    /* IPS   = 101（48位物理地址空间）*/
    msr     tcr_el1, x0

    /*
     * isb：指令同步屏障
     * 确保后续取指令使用新的 MAIR/TCR 配置。
     * 参考：ARMv8-A ARM Section B2.3.6
     */
    isb
    ret

/*
 * ============================================================
 * enable_mmu - 开启 MMU
 *
 * 参数（AArch64 C ABI，参考AAPCS64）：
 *   x0 = TTBR0_EL1 的值（早期页表 PGD 的物理地址）
 *
 * 操作顺序：
 *   1. 写 TTBR0_EL1（恒等映射页表基址）
 *   2. 写 TTBR1_EL1（高VA页表基址，Phase 2 设为 0 = 无映射）
 *   3. dsb ish：数据同步屏障，确保页表写入对硬件可见
 *   4. isb：指令同步屏障
 *   5. 读改写 SCTLR_EL1，置位 M（MMU使能）、C（D-Cache）、I（I-Cache）
 *   6. isb：等待新的 SCTLR 生效
 *   7. 执行 TLB 全局无效化
 *   8. ret
 *
 * 关键：步骤5之后指令仍在恒等映射的低VA继续执行（VA == PA），
 * 所以 ret 正常返回到 mmu_init() 调用处。
 *
 * C 原型：void enable_mmu(unsigned long pgd_phys);
 * ============================================================
 */
    .global enable_mmu
enable_mmu:
    /*
     * Step 1: 写 TTBR0_EL1
     * x0 = PGD物理地址（由 create_page_tables 返回，必须4KB对齐）
     *
     * TTBR0_EL1格式：bits[47:1] = BADDR（页表基址），bit[0] = CnP（Common not Private）
     * 直接写物理地址即可（4KB对齐保证低12位为0）。
     */
    msr     ttbr0_el1, x0

    /*
     * Step 2: 写 TTBR1_EL1
     * Phase 2: 内核暂不使用高VA，设为0（无有效映射）。
     * 若代码访问高VA地址，会触发Translation Fault（这是预期行为）。
     */
    msr     ttbr1_el1, xzr

    /*
     * Step 3 & 4: 内存屏障
     * dsb ish：等待之前所有内存访问完成（包括页表写入）
     * isb：刷新指令流水线，确保后续指令使用新的TTBR配置
     */
    dsb     ish
    isb

    /*
     * Step 5: 读改写 SCTLR_EL1 开启 MMU
     *
     * SCTLR_EL1 关键位（参考 ARMv8-A ARM Section D13.2.113）：
     *   bit 0  (M):   MMU使能，0→1开启地址翻译
     *   bit 2  (C):   Data Cache使能
     *   bit 12 (I):   Instruction Cache使能
     *
     * 注意：同时开启 M+C+I。Phase 2 为简化不分开设置。
     * 若系统不稳定，可先只设 M，观察后再开 C/I。
     */
    mrs     x1, sctlr_el1
    orr     x1, x1, #(1 << 0)      /* M: MMU enable */
    orr     x1, x1, #(1 << 2)      /* C: D-cache enable */
    orr     x1, x1, #(1 << 12)     /* I: I-cache enable */
    msr     sctlr_el1, x1

    /*
     * Step 6: isb — 确保 SCTLR_EL1 写入立即生效
     * 此指令之后，CPU开始通过 TTBR0/TTBR1 进行地址翻译。
     * 由于恒等映射，下一条指令 (tlbi...) 的VA == PA，翻译成功。
     */
    isb

    /*
     * Step 7: TLB 全局无效化
     *
     * MMU 开启时 TLB 可能含有旧条目（来自之前的某次运行）。
     * TLBI VMALLE1：无效化 EL1 的所有 TLB 条目（不区分 ASID）。
     * dsb ish + isb：确保 TLB 无效化完成后再继续。
     */
    tlbi    vmalle1
    dsb     ish
    isb

    ret
