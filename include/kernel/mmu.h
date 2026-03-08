/*
 * include/kernel/mmu.h — KAI OS MMU interface
 *
 * Hardware-enforced memory isolation for the AI sandbox.
 *
 * Architecture: ARMv8-A AArch64
 * Granule:      4 KB
 * VA size:      39-bit (512 GB, sufficient for QEMU virt)
 * Table levels: L1 (512 entries, 1 GB each) + L2 (512 entries, 2 MB each)
 * We use block descriptors at L2 — no L3 needed for 2 MB
 * aligned regions. The sandbox scratch page is small enough
 * to warrant an L3 table for 4 KB granularity.
 *
 * Address space:
 * TTBR1_EL1 — kernel VA space  (0xFFFF_xxxx_xxxx_xxxx) — EL1 only
 * TTBR0_EL1 — EL0 VA space     (0x0000_xxxx_xxxx_xxxx) — kernel + EL0
 *
 * Security model:
 * All kernel code, data, stack, and MMIO are mapped via TTBR1 (EL1-only).
 * The sandbox scratch page is additionally mapped via TTBR0 so EL0 can
 * read and write it. EL0 cannot see kernel pages — the hardware enforces
 * this because TTBR0 only contains the scratch mapping.
 * If EL0 code attempts to access any other address it will fault and
 * the EL1 sync exception handler will catch it.
 */

#ifndef KERNEL_MMU_H
#define KERNEL_MMU_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Page size and alignment -------------------------------------------- */
#define PAGE_SIZE       4096U
#define PAGE_MASK       (~(uint64_t)(PAGE_SIZE - 1U))
#define PAGE_ALIGN(x)   (((uint64_t)(x) + PAGE_SIZE - 1U) & PAGE_MASK)

/* ---- AArch64 descriptor attribute bits (Table D5-15 through D5-21) ------ */

/* Validity and type */
#define PTE_VALID       (1ULL << 0)   /* descriptor is valid          */
#define PTE_TABLE       (1ULL << 1)   /* points to next-level table   */
#define PTE_BLOCK       (0ULL << 1)   /* block/page descriptor        */
#define PTE_PAGE        (1ULL << 1)   /* page descriptor (L3 only)    */

/* Memory Attribute Indirection Register index (MAIR_EL1) */
#define PTE_ATTR_NORMAL    (0ULL << 2)   /* MAIR index 0 = Normal WB     */
#define PTE_ATTR_DEVICE    (1ULL << 2)   /* MAIR index 1 = Device nGnRnE */
#define PTE_ATTR_NORMAL_NC (2ULL << 2)   /* MAIR index 2 = Normal NC     */

/* Shareability */
#define PTE_SH_NONE     (0ULL << 8)
#define PTE_SH_OUTER    (2ULL << 8)
#define PTE_SH_INNER    (3ULL << 8)

/* Access flag — must be set or hardware will fault on first access */
#define PTE_AF          (1ULL << 10)

/* Access permissions */
#define PTE_AP_EL1_RW   (0ULL << 6)   /* EL1 RW,  EL0 none            */
#define PTE_AP_RW       (1ULL << 6)   /* EL1 RW,  EL0 RW              */
#define PTE_AP_EL1_RO   (2ULL << 6)   /* EL1 RO,  EL0 none            */
#define PTE_AP_RO       (3ULL << 6)   /* EL1 RO,  EL0 RO              */

/* Privileged Execute Never / Execute Never */
#define PTE_PXN         (1ULL << 53)  /* EL1 cannot execute           */
#define PTE_UXN         (1ULL << 54)  /* EL0 cannot execute           */
#define PTE_XN          (PTE_PXN | PTE_UXN)

/* ---- Composite attribute sets for common use cases --------------------- */

/*
 * Composite attribute sets for L2 BLOCK descriptors (2 MB mappings).
 *
 * IMPORTANT: Do NOT include PTE_PAGE (bit 1). At L2, bit[1]=0 = block.
 *
 * We use Normal WB inner-shareable for kernel memory. This is safe
 * because mmu_enable_asm enables the MMU first with these descriptors,
 * then immediately invalidates and enables caches before returning.
 * By the time C code runs after mmu_enable(), caches are fully active.
 *
 * Device memory stays nGnRnE + non-shareable — always correct for MMIO.
 */

/* Kernel RW, EL1 only, no execute, normal WB inner-shareable */
#define PTE_KERNEL_RW  (PTE_VALID | PTE_BLOCK | PTE_AF | \
                        PTE_ATTR_NORMAL_NC | PTE_SH_NONE | \
                        PTE_AP_EL1_RW | PTE_XN)

/* Kernel RX, EL1 only, normal WB inner-shareable */
#define PTE_KERNEL_RX  (PTE_VALID | PTE_BLOCK | PTE_AF | \
                        PTE_ATTR_NORMAL_NC | PTE_SH_NONE | \
                        PTE_AP_EL1_RO | PTE_UXN)

#define PTE_KERNEL_RWX (PTE_VALID | PTE_BLOCK | PTE_AF | \
                        PTE_ATTR_NORMAL_NC | PTE_SH_NONE | \
                        PTE_AP_EL1_RW)

/* EL0+EL1 RW, sandbox scratch, normal WB inner-shareable, no execute */
#define PTE_EL0_RW     (PTE_VALID | PTE_BLOCK | PTE_AF | \
                        PTE_ATTR_NORMAL_NC | PTE_SH_NONE | \
                        PTE_AP_RW | PTE_XN)

/* Device memory (MMIO) — EL1 only, no execute, device nGnRnE */
#define PTE_DEVICE_RW  (PTE_VALID | PTE_BLOCK | PTE_AF | \
                        PTE_ATTR_DEVICE | PTE_SH_NONE | \
                        PTE_AP_EL1_RW | PTE_XN)

/* ---- MAIR_EL1 encoding -------------------------------------------------- */
/*
 * Index 0: Normal Inner/Outer Write-Back, Read-Allocate, Write-Allocate (0xFF)
 * Index 1: Device nGnRnE (0x00) — Most restrictive, safest for UART
 *
 * We encode both upfront. The CPU only uses the index referenced by a
 * descriptor's AttrIdx field, so unused indices are harmless.
 */
#define MAIR_NORMAL_WB  0xFFULL   /* index 0: Normal Inner/Outer WB RAWA  */
#define MAIR_DEVICE     0x00ULL   /* index 1: Device nGnRnE               */
#define MAIR_NORMAL_NC  0x44ULL   /* index 2: Normal Inner/Outer NC       */
#define MAIR_VALUE      (MAIR_NORMAL_WB | (MAIR_DEVICE << 8) | (MAIR_NORMAL_NC << 16))

/* ---- TCR_EL1 encoding --------------------------------------------------- */
/*
 * TCR_EL1 — absolute minimum for a working identity map.
 *
 * T0SZ=32  → 32-bit VA range via TTBR0 (covers 0x00000000–0xFFFFFFFF)
 * TG0=00   → 4 KB granule
 * EPD1=1   → disable TTBR1 walks entirely
 * IPS=001  → 36-bit physical address (enough for QEMU virt)
 *
 * SH0=3 (Inner Shareable), IRGN0=1, ORGN0=1 = Write-Back Write-Allocate.
 * This is the standard configuration for ARMv8 systems.
 */
#define TCR_T0SZ        (32ULL << 0)
#define TCR_IRGN0_WBWA  (1ULL  << 8)
#define TCR_ORGN0_WBWA  (1ULL  << 10)
#define TCR_SH0_INNER   (3ULL  << 12)
#define TCR_TG0_4K      (0ULL  << 14)
#define TCR_EPD1        (1ULL  << 23)
#define TCR_IPS_36BIT   (1ULL  << 32)

#define TCR_VALUE       (TCR_T0SZ | TCR_IRGN0_WBWA | TCR_ORGN0_WBWA | \
                         TCR_SH0_INNER | TCR_TG0_4K | TCR_EPD1 | TCR_IPS_36BIT)

/* ---- Linker-exported region symbols (defined in linker.ld) -------------- */
extern char __page_tables_start[];
extern char __page_tables_end[];
extern char __sandbox_scratch_start[];
extern char __sandbox_scratch_end[];
extern char __kernel_end[];

/* ---- Public API --------------------------------------------------------- */

void mmu_init(void);
void mmu_enable(void);
void vbar_install(void);

/*
 * mmu_map_page — map a single 4 KB page.
 * Returns true on success.
 */
bool mmu_map_page(uint64_t *ttbr, uint64_t va, uint64_t pa, uint64_t attrs);

#ifdef __cplusplus
}
#endif

#endif /* KERNEL_MMU_H */