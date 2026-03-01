/*
 * scripts/linker.ld — AArch64 bare-metal linker script
 *
 * Target: QEMU -M virt
 * Load address: 0x40000000  (default QEMU virt kernel load point)
 *
 * Memory layout (low → high):
 *
 *   0x40000000  .text       — code (boot entry first)
 *               .rodata     — read-only data
 *               .data       — initialised data
 *               .bss        — zero-initialised data
 *               [4 KB guard page — catches stack overflow before it hits BSS]
 *               [64 KB stack region, grows downward]
 *   __stack_top              — sp initialised here by boot.S
 */

ENTRY(_start)

SECTIONS
{
    . = 0x40000000;

    /* ------------------------------------------------------------------ *
     * .text — executable code.  Boot stub must come first so that the    *
     * image entry point is exactly at the load address.                  *
     * ------------------------------------------------------------------ */
    .text : {
        KEEP(*(.text.boot))
        *(.text .text.*)
    }

    /* ------------------------------------------------------------------ *
     * .rodata — constants, string literals, jump tables.                 *
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
     *                                                                    *
     * PROVIDE() prevents a linker error when the section is empty.       *
     * boot.S zeroes the region [__bss_start, __bss_end) using 8-byte    *
     * stores, so both symbols must be 8-byte aligned.                    *
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
     * Guard page — 4 KB of address space intentionally left unmapped.    *
     * A stack overflow will fault here rather than silently trashing BSS.*
     * ------------------------------------------------------------------ */
    . = ALIGN(4096);
    PROVIDE(__guard_bottom = .);
    . = . + 0x1000;         /* 4 KB */
    PROVIDE(__guard_top = .);

    /* ------------------------------------------------------------------ *
     * Stack — 64 KB, growing downward from __stack_top.                 *
     * ------------------------------------------------------------------ */
    . = ALIGN(16);
    PROVIDE(__stack_bottom = .);
    . = . + 0x10000;        /* 64 KB */
    PROVIDE(__stack_top = .);

    /* ------------------------------------------------------------------ *
     * Discard unwanted sections that toolchains sometimes emit.          *
     * Keeps the final binary clean and avoids RWX segment warnings.      *
     * ------------------------------------------------------------------ */
    /DISCARD/ : {
        *(.comment)
        *(.gnu.attributes)
        *(.note.gnu.build-id)
        *(.eh_frame)
        *(.ARM.exidx*)
        *(.ARM.extab*)
    }
}
