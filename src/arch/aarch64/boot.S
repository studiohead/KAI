/*
 * boot.S — AArch64 bare-metal entry point
 *
 * Responsibilities:
 *   1. Park all secondary cores safely
 *   2. Initialise the stack pointer from a linker-defined symbol
 *   3. Zero the BSS segment using pointer comparison (no word-count drift)
 *   4. Jump to kernel_main — never return
 */

.section ".text.boot"
.global _start

_start:
    /* ------------------------------------------------------------------ *
     * Only core 0 continues; all others spin in a low-power wait loop.   *
     * ------------------------------------------------------------------ */
    mrs     x0, mpidr_el1
    and     x0, x0, #0xFF
    cbz     x0, .Lmaster

.Lhang:
    wfe
    b       .Lhang

.Lmaster:
    /* ------------------------------------------------------------------ *
     * Stack pointer — grows downward from __stack_top.                   *
     * __stack_top is defined in the linker script, above a 64 KB guard   *
     * region that separates the stack from .bss.                         *
     * ------------------------------------------------------------------ */
    ldr     x0, =__stack_top
    mov     sp, x0

    /* ------------------------------------------------------------------ *
     * Zero BSS using start/end pointer comparison.                       *
     * This avoids the word-count drift bug present in size-based loops.  *
     * Both symbols are 16-byte aligned by the linker script.             *
     * ------------------------------------------------------------------ */
    ldr     x1, =__bss_start
    ldr     x2, =__bss_end

.Lzero_bss:
    cmp     x1, x2
    b.ge    .Lrun_main
    str     xzr, [x1], #8
    b       .Lzero_bss

.Lrun_main:
    /* Full memory barrier before handing off to C */
    dsb     sy
    isb

    bl      kernel_main

    /* kernel_main should never return; park the core if it does */
    b       .Lhang
