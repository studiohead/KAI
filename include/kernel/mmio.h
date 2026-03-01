/*
 * include/kernel/mmio.h — Memory-mapped I/O helpers
 *
 * Provides typed, volatile-correct accessors so that raw casts are never
 * scattered throughout driver or kernel code.
 */

#ifndef KERNEL_MMIO_H
#define KERNEL_MMIO_H

#include <stdint.h>

static inline void mmio_write32(uintptr_t addr, uint32_t val)
{
    *(volatile uint32_t *)addr = val;
}

static inline uint32_t mmio_read32(uintptr_t addr)
{
    return *(volatile uint32_t *)addr;
}

static inline void mmio_write64(uintptr_t addr, uint64_t val)
{
    *(volatile uint64_t *)addr = val;
}

static inline uint64_t mmio_read64(uintptr_t addr)
{
    return *(volatile uint64_t *)addr;
}

#endif /* KERNEL_MMIO_H */
