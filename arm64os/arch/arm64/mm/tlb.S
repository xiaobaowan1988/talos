/* SPDX-License-Identifier: GPL-2.0 */
/*
 * arch/arm64/mm/tlb.S
 *
 * TLB maintenance operations
 * Reference: arch/arm64/include/asm/tlbflush.h
 */

    .section ".text", "ax"

/*
 * tlb_flush_all — invalidate all TLB entries for EL1
 *
 * Sequence: DSB → TLBI → DSB → ISB
 *   dsb ishst : ensure all prior stores (page table writes) are visible
 *   tlbi vmalle1 : invalidate all EL1 TLB entries (local CPU)
 *   dsb ish   : ensure TLB invalidation completes
 *   isb       : synchronize instruction stream
 *
 * For multi-core (future): use vmalle1is (inner-shareable broadcast).
 * Phase 2 is single-core, vmalle1 suffices.
 */
    .global tlb_flush_all
tlb_flush_all:
    dsb     ishst
    tlbi    vmalle1
    dsb     ish
    isb
    ret
