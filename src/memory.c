/*
 * src/memory.c — Memory region whitelist and safe access helpers
 *
 * Defines the whitelisted memory regions used by syscalls to validate
 * addresses before allowing reads or writes. The linker symbols
 * __bss_start, __bss_end, __stack_bottom, __stack_top are defined in
 * scripts/linker.ld and resolved at link time.
 *
 * Rule: headers declare, .c files define.
 * memory.h holds extern declarations; this file holds the actual definitions.
 */

#include <kernel/memory.h>
#include <kernel/syscall.h>

/* ---- Linker symbol references ----------------------------------------
 * Declared as char arrays — the correct freestanding way to reference
 * linker-defined symbols. Using the array name directly gives the address
 * without needing & (which would be UB on a scalar extern declaration).
 * -------------------------------------------------------------------- */
extern char __bss_start[];
extern char __bss_end[];
extern char __stack_bottom[];
extern char __stack_top[];

/* ---- Whitelisted memory regions --------------------------------------
 * Any address passed to sys_mem_read must fall within one of these
 * regions. Initialised at definition time from linker symbols — safe
 * because the linker resolves these before any C code runs.
 *
 * To add a sandbox scratch buffer later:
 *   { (uintptr_t)scratch_buf, (uintptr_t)(scratch_buf + SCRATCH_SIZE) },
 * -------------------------------------------------------------------- */
mem_region_t memory_regions[] = {
    { (uintptr_t)__bss_start,    (uintptr_t)__bss_end   },  /* BSS        */
    { (uintptr_t)__stack_bottom, (uintptr_t)__stack_top },  /* Stack      */
};

const size_t memory_region_count =
    sizeof(memory_regions) / sizeof(memory_regions[0]);

/* ---- sys_mem_info -----------------------------------------------------
 * Fills caller-provided pointers with BSS and stack boundary addresses.
 * Requires CAP_READ_MEM. Returns 0 on success, -1 on failure.
 * -------------------------------------------------------------------- */
int sys_mem_info(uintptr_t *bss_start, uintptr_t *bss_end,
                 uintptr_t *stack_start, uintptr_t *stack_end,
                 uint32_t caller_caps)
{
    /* Capability check */
    if (!(caller_caps & CAP_READ_MEM))
        return -1;

    /* NULL pointer guard */
    if (!bss_start || !bss_end || !stack_start || !stack_end)
        return -1;

    *bss_start   = (uintptr_t)__bss_start;
    *bss_end     = (uintptr_t)__bss_end;
    *stack_start = (uintptr_t)__stack_bottom;
    *stack_end   = (uintptr_t)__stack_top;

    return 0;
}

/* ---- sys_mem_read -----------------------------------------------------
 * Copies 'len' bytes from src_addr into dst_buf, but only if every byte
 * of the source range falls within a whitelisted region.
 * Requires CAP_READ_MEM. Returns 0 on success, -1 on failure.
 * -------------------------------------------------------------------- */
int sys_mem_read(uintptr_t src_addr, void *dst_buf, size_t len,
                 uint32_t caller_caps)
{
    /* Capability check */
    if (!(caller_caps & CAP_READ_MEM))
        return -1;

    /* NULL destination guard */
    if (!dst_buf || len == 0)
        return -1;

    /* Verify the entire requested range is within a whitelisted region.
     * We check both start and end-1 to guard against wrap-around. */
    uintptr_t src_end = src_addr + len;
    if (src_end < src_addr)   /* overflow check */
        return -1;

    int allowed = 0;
    for (size_t i = 0; i < memory_region_count; i++) {
        if (src_addr >= memory_regions[i].start &&
            src_end  <= memory_regions[i].end) {
            allowed = 1;
            break;
        }
    }

    if (!allowed)
        return -1;

    /* Safe to copy */
    const uint8_t *src = (const uint8_t *)src_addr;
    uint8_t       *dst = (uint8_t *)dst_buf;
    for (size_t i = 0; i < len; i++)
        dst[i] = src[i];

    return 0;
}