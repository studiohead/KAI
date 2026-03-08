/*
 * src/arch/aarch64/mmu.S — MMU enable, EL0 entry/exit, exception vectors
 *
 * Functions exported:
 * mmu_enable_asm   — write system registers and set SCTLR_EL1.M
 * el0_enter        — drop from EL1 to EL0 for sandbox execution
 * __el0_trampoline — EL0 entry stub (returns to EL1 via SVC)
 * el0_sync_vector  — EL1 sync exception handler (catches EL0 SVC/faults)
 * irq_handler      — EL1 IRQ handler (calls irq_dispatch in C)
 * vbar_install     — write VBAR_EL1 to point at kai_vector_table
 *
 * EL0 return address note:
 * .Lel0_return is a label inside el0_enter after the eret instruction.
 * el0_sync_vector must jump back there.  We store the address in a
 * global symbol (el0_return_addr) in .data so the linker resolves it
 * at link time — no runtime PC-relative tricks needed.
 */

.section ".text"

/* ======================================================================
 * mmu_enable_asm(ttbr0, ttbr1_unused, mair, tcr)
 *
 * x0 = TTBR0_EL1
 * x1 = ignored
 * x2 = MAIR_EL1
 * x3 = TCR_EL1
 * ====================================================================== */
.global mmu_enable_asm
mmu_enable_asm:
    /*
     * Step 0: Disable the MMU first.
     * QEMU -cpu max boots with SCTLR_EL1.M already set.
     * We must turn it off, configure our tables, then re-enable.
     * Writing TCR/TTBR while the MMU is live causes an immediate fault.
     */
    mrs     x4, sctlr_el1
    bic     x4, x4, #(1 << 0)   /* M = 0 (MMU off)     */
    bic     x4, x4, #(1 << 2)   /* C = 0 (D-cache off) */
    bic     x4, x4, #(1 << 12)  /* I = 0 (I-cache off) */
    msr     sctlr_el1, x4
    isb

    /* Set up translation control registers */
    msr     mair_el1,  x2
    msr     tcr_el1,   x3
    msr     ttbr0_el1, x0
    mov     x4, #0
    msr     ttbr1_el1, x4
    isb

    /* Flush TLBs to ensure no stale translations exist */
    tlbi    vmalle1
    dsb     sy
    isb

    /* Step 1: Enable MMU only — pages are NC so D-cache off is safe here */
    mrs     x4, sctlr_el1
    orr     x4, x4, #(1 << 0)   /* M = 1 (MMU on)              */
    bic     x4, x4, #(1 << 2)   /* C = 0 (D-cache still off)   */
    bic     x4, x4, #(1 << 1)   /* A = 0 (no alignment faults) */
    msr     sctlr_el1, x4
    isb                          /* MMU now live                 */

    /* Step 2: Invalidate I-cache before enabling it */
    ic      iallu
    dsb     ish
    isb

    /* Step 3: Enable D-cache and I-cache now that they are clean */
    mrs     x4, sctlr_el1
    orr     x4, x4, #(1 << 2)   /* C = 1 (D-cache on) */
    orr     x4, x4, #(1 << 12)  /* I = 1 (I-cache on) */
    msr     sctlr_el1, x4
    isb

    /* Step 4: Flush TLBs */
    tlbi    vmalle1
    dsb     ish
    isb

    /* Breadcrumb: 'M' on UART = MMU + caches active, no fault */
    ldr     x5, =0x09000000
    mov     x6, #0x4D           /* 'M' */
    str     x6, [x5]
    mov     x6, #0x0D
    str     x6, [x5]
    mov     x6, #0x0A
    str     x6, [x5]

    ret

/* ======================================================================
 * el0_enter(entry_fn, arg0) → sandbox_result_t
 * ====================================================================== */
.global el0_enter
el0_enter:
    /* Save callee-saved registers + LR */
    stp     x29, x30, [sp, #-16]!
    stp     x19, x20, [sp, #-16]!
    stp     x21, x22, [sp, #-16]!
    stp     x23, x24, [sp, #-16]!

    /* Stash args in callee-saved regs across the eret */
    mov     x22, x0     /* entry_fn */
    mov     x23, x1     /* arg0     */

    /* Set EL0 stack pointer */
    ldr     x19, =__el0_stack_top
    msr     sp_el0, x19

    /* SPSR_EL1 = 0 → return to EL0t, all interrupts unmasked */
    mov     x20, #0
    msr     spsr_el1, x20

    /* ELR_EL1 = __el0_trampoline → that's where eret jumps */
    ldr     x21, =__el0_trampoline
    msr     elr_el1, x21

    /* Pass entry_fn and arg0 to the trampoline */
    mov     x0, x22
    mov     x1, x23

    eret

    /* Resume here after SVC #0 */
.Lel0_return:
    ldp     x23, x24, [sp], #16
    ldp     x21, x22, [sp], #16
    ldp     x19, x20, [sp], #16
    ldp     x29, x30, [sp], #16
    ret

.section ".data"
.global el0_return_addr
.balign 8
el0_return_addr:
    .quad   .Lel0_return

.section ".text"

/* ======================================================================
 * __el0_trampoline — runs at EL0
 * ====================================================================== */
.global __el0_trampoline
__el0_trampoline:
    blr     x0
    svc     #0
.Ltrampoline_hang:
    wfe
    b       .Ltrampoline_hang

/* ======================================================================
 * Exception vector table — VBAR_EL1
 * 2KB aligned, 16 slots of 128 bytes each.
 * ====================================================================== */
    .balign 2048
.global kai_vector_table
kai_vector_table:
    /* Current EL / SP0 */
    .balign 128
    b   .Lvec_hang
    .balign 128
    b   .Lvec_hang
    .balign 128
    b   .Lvec_hang
    .balign 128
    b   .Lvec_hang

    /* Current EL / SPx */
    .balign 128
    b   .Lvec_hang
    .balign 128
    b   irq_handler
    .balign 128
    b   .Lvec_hang
    .balign 128
    b   .Lvec_hang

    /* Lower EL (AArch64) — EL0 exceptions land here */
    .balign 128
    b   el0_sync_vector
    .balign 128
    b   .Lvec_hang
    .balign 128
    b   .Lvec_hang
    .balign 128
    b   .Lvec_hang

    /* Lower EL (AArch32) — not used */
    .balign 128
    b   .Lvec_hang
    .balign 128
    b   .Lvec_hang
    .balign 128
    b   .Lvec_hang
    .balign 128
    b   .Lvec_hang

/* ======================================================================
 * el0_sync_vector — handles synchronous exceptions from EL0
 * ====================================================================== */
.global el0_sync_vector
el0_sync_vector:
    mrs     x10, esr_el1
    ubfx    x10, x10, #26, #6       /* extract EC field */
    cmp     x10, #0x15              /* 0x15 = SVC64     */
    b.eq    .Ldo_el1_return

    mov     x0, #4                  /* SANDBOX_ERR_FAULT */

.Ldo_el1_return:
    ldr     x11, =el0_return_addr
    ldr     x11, [x11]
    msr     elr_el1, x11
    mov     x12, #0x05              /* EL1h */
    msr     spsr_el1, x12
    eret

/* ======================================================================
 * irq_handler
 * ====================================================================== */
.global irq_handler
irq_handler:
    stp     x29, x30, [sp, #-16]!
    stp     x0,  x1,  [sp, #-16]!
    stp     x2,  x3,  [sp, #-16]!
    bl      irq_dispatch
    ldp     x2,  x3,  [sp], #16
    ldp     x0,  x1,  [sp], #16
    ldp     x29, x30, [sp], #16
    eret

.Lvec_hang:
    wfe
    b   .Lvec_hang

/* ======================================================================
 * vbar_install — write VBAR_EL1
 * ====================================================================== */
.global vbar_install
vbar_install:
    ldr     x0, =kai_vector_table
    msr     vbar_el1, x0
    isb
    ret