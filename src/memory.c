/*
 * src/memory.c — Memory region whitelist and safe access helpers
 *
 * memory_regions[] is populated at runtime by memory_init() because
 * freestanding C cannot use linker symbols in static initialisers —
 * they are not constant expressions the compiler can evaluate at
 * compile time.  Call memory_init() from kernel_main before any
 * sandbox or verifier code runs.
 */

#include <kernel/memory.h>
#include <kernel/syscall.h>
#include <stddef.h>
#include <stdint.h>

/* ---- Linker symbols ---------------------------------------------------- */
extern char __bss_start[];
extern char __bss_end[];
extern char __stack_bottom[];
extern char __stack_top[];
extern char __sandbox_scratch_start[];
extern char __sandbox_scratch_end[];

/* ---- Whitelisted regions (populated at runtime by memory_init) --------- */
mem_region_t memory_regions[MEMORY_REGION_MAX];
size_t       memory_region_count = 0;

/* ======================================================================
 * memory_init — populate the whitelist from linker symbols
 * Must be called once from kernel_main before any sandbox use.
 * ====================================================================== */
void memory_init(void)
{
    memory_regions[0].start = (uintptr_t)__bss_start;
    memory_regions[0].end   = (uintptr_t)__bss_end;

    memory_regions[1].start = (uintptr_t)__stack_bottom;
    memory_regions[1].end   = (uintptr_t)__stack_top;

    memory_regions[2].start = (uintptr_t)__sandbox_scratch_start;
    memory_regions[2].end   = (uintptr_t)__sandbox_scratch_end;

    memory_region_count = 3;
}

/* ======================================================================
 * sys_mem_info
 * ====================================================================== */
int sys_mem_info(uintptr_t *bss_start, uintptr_t *bss_end,
                 uintptr_t *stack_start, uintptr_t *stack_end,
                 uint32_t caller_caps)
{
    if (!(caller_caps & CAP_READ_MEM)) return -1;
    if (!bss_start || !bss_end || !stack_start || !stack_end) return -1;

    *bss_start   = (uintptr_t)__bss_start;
    *bss_end     = (uintptr_t)__bss_end;
    *stack_start = (uintptr_t)__stack_bottom;
    *stack_end   = (uintptr_t)__stack_top;

    return 0;
}

/* ======================================================================
 * sys_mem_read
 * ====================================================================== */
int sys_mem_read(uintptr_t src_addr, void *dst_buf, size_t len,
                 uint32_t caller_caps)
{
    if (!(caller_caps & CAP_READ_MEM)) return -1;
    if (!dst_buf || len == 0) return -1;

    uintptr_t src_end = src_addr + len;
    if (src_end < src_addr) return -1;   /* overflow */

    int allowed = 0;
    for (size_t i = 0; i < memory_region_count; i++) {
        if (src_addr >= memory_regions[i].start &&
            src_end  <= memory_regions[i].end) {
            allowed = 1;
            break;
        }
    }
    if (!allowed) return -1;

    const uint8_t *src = (const uint8_t *)src_addr;
    uint8_t       *dst = (uint8_t *)dst_buf;
    for (size_t i = 0; i < len; i++) dst[i] = src[i];

    return 0;
}