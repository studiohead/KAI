// include/kernel/memory.h
// KAI OS: Memory region whitelist for safe syscalls

#ifndef KAI_MEMORY_H
#define KAI_MEMORY_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// -----------------------------------------------------------------------------
// Memory region structure
// Defines a start (inclusive) and end (exclusive) for a safe memory range
// -----------------------------------------------------------------------------
typedef struct {
    uintptr_t start;
    uintptr_t end;
} mem_region_t;

// -----------------------------------------------------------------------------
// Symbols from the linker defining BSS and stack
// These are used to allow safe reads/writes only in whitelisted regions
// -----------------------------------------------------------------------------
extern char __bss_start[];
extern char __bss_end[];
extern char __stack_bottom[];
extern char __stack_top[];

// -----------------------------------------------------------------------------
// Whitelisted memory regions
// Add any additional safe regions here (shared buffers, MMIO if safe, etc.)
// -----------------------------------------------------------------------------
extern mem_region_t memory_regions[];
extern const size_t memory_region_count;

// -----------------------------------------------------------------------------
// Helper function: check if an address is in any allowed region
// Returns 1 if allowed, 0 if not
// -----------------------------------------------------------------------------
static inline int is_allowed_addr(uintptr_t addr)
{
    for (size_t i = 0; i < memory_region_count; i++) {
        if (addr >= memory_regions[i].start && addr < memory_regions[i].end) {
            return 1;
        }
    }
    return 0;
}

#ifdef __cplusplus
}
#endif

#endif // KAI_MEMORY_H