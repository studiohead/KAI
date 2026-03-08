/*
 * scripts/linker.ld — AArch64 bare-metal linker script
 *
 * Target: QEMU -M virt
 * Load address: 0x40000000  (default QEMU virt kernel load point)
 */

ENTRY(_start)

SECTIONS
{
    /* The kernel starts at 1GB in the physical address space */
    . = 0x40000000;

    /* ------------------------------------------------------------------ *
     * .text — executable code.  Boot stub first.                         *
     * ------------------------------------------------------------------ */
    .text : {
        KEEP(*(.text.boot))
        *(.text .text.*)
    }

    /* ------------------------------------------------------------------ *
     * .rodata — constants and string literals.                            *
     * ------------------------------------------------------------------ */
    .rodata : {
        . = ALIGN(16);
        *(.rodata .rodata.*)
    }

    /* ------------------------------------------------------------------ *
     * .data — mutable, pre-initialised globals.                          *
     * ------------------------------------------------------------------ */
    .data : {
        . = ALIGN(16);
        *(.data .data.*)
    }

    /* ------------------------------------------------------------------ *
     * .bss — zero-initialised globals.                                   *
     * ------------------------------------------------------------------ */
    .bss (NOLOAD) : {
        . = ALIGN(16);
        PROVIDE(__bss_start = .);
        *(.bss .bss.*)
        *(COMMON)
        . = ALIGN(16);
        PROVIDE(__bss_end = .);
    }

    /* ------------------------------------------------------------------ *
     * Kernel stack + guard pages.                                        *
     * ------------------------------------------------------------------ */
    . = ALIGN(4096);
    PROVIDE(__guard_bottom = .);
    . = . + 0x1000;
    PROVIDE(__guard_top = .);

    . = ALIGN(16);
    PROVIDE(__stack_bottom = .);
    . = . + 0x10000;
    PROVIDE(__stack_top = .);

    /* ------------------------------------------------------------------ *
     * EL0 sandbox stack.                                                 *
     * ------------------------------------------------------------------ */
    . = ALIGN(4096);
    PROVIDE(__el0_guard_bottom = .);
    . = . + 0x1000;
    
    . = ALIGN(4096);
    PROVIDE(__el0_stack_bottom = .);
    . = . + 0x4000;
    PROVIDE(__el0_stack_top = .);

    . = ALIGN(4096);
    PROVIDE(__el0_stack_guard = .);
    . = . + 0x1000;

    /* ------------------------------------------------------------------ *
     * .page_tables — MMU translation tables.                             *
     * * * Base must be 4KB aligned for TTBR0_EL1 to accept the pointer.
     * ------------------------------------------------------------------ */
    . = ALIGN(4096);
    .page_tables (NOLOAD) : {
        PROVIDE(__page_tables_start = .);
        . = . + 0x3000;
        PROVIDE(__page_tables_end = .);
    }

    /* * MARKER: The kernel "private" area ends here. 
     * This allows mmu.c to map everything before this as EL1-only.
     */
    . = ALIGN(16);
    PROVIDE(__kernel_end = .);

    /* ------------------------------------------------------------------ *
     * .sandbox_scratch — THE SECURITY BOUNDARY                           *
     * *
     * We ALIGN(0x200000) (2 MB) so that this section starts at the       *
     * beginning of a new L2 Block Descriptor. This ensures that the      *
     * ATTR_EL0_DATA attributes applied in mmu.c do not accidentally      *
     * cover kernel code, stacks, or private data.                       *
     * ------------------------------------------------------------------ */
    . = ALIGN(0x200000); 
    .sandbox_scratch (NOLOAD) : {
        PROVIDE(__sandbox_scratch_start = .);
        /* We reserve exactly 2MB to fill the hardware block descriptor */
        . = . + 0x200000;
        PROVIDE(__sandbox_scratch_end = .);
    }

    /* Final alignment padding to keep the image clean */
    . = ALIGN(0x200000);

    /DISCARD/ : {
        *(.comment)
        *(.gnu.attributes)
        *(.note.gnu.build-id)
        *(.eh_frame)
        *(.ARM.exidx*)
        *(.ARM.extab*)
    }
}