// include/kernel/memory.h — Memory region whitelist for KAI OS sandbox

#ifndef KAI_MEMORY_H
#define KAI_MEMORY_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MEMORY_REGION_MAX 8   /* maximum number of whitelisted regions */

typedef struct {
    uintptr_t start;   /* inclusive */
    uintptr_t end;     /* exclusive */
} mem_region_t;

/* Populated at runtime by memory_init() */
extern mem_region_t memory_regions[MEMORY_REGION_MAX];
extern size_t       memory_region_count;

/* Must be called once from kernel_main before any sandbox use */
void memory_init(void);

/* Syscall declarations */
int sys_mem_info(uintptr_t *bss_start, uintptr_t *bss_end,
                 uintptr_t *stack_start, uintptr_t *stack_end,
                 uint32_t caller_caps);

int sys_mem_read(uintptr_t src_addr, void *dst_buf, size_t len,
                 uint32_t caller_caps);

/* Check if a single address falls within any whitelisted region */
static inline int is_allowed_addr(uintptr_t addr)
{
    for (size_t i = 0; i < memory_region_count; i++) {
        if (addr >= memory_regions[i].start && addr < memory_regions[i].end)
            return 1;
    }
    return 0;
}

#ifdef __cplusplus
}
#endif

#endif /* KAI_MEMORY_H */