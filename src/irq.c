/*
 * src/irq.c — KAI OS interrupt-driven pipeline engine
 *
 * Implements the GIC-400 driver and the IRQ → pipeline dispatch table.
 *
 * Interrupt-driven pipelines give the AI "reflexes": sensor events fire
 * hardware interrupts which immediately execute pre-verified AIQL pipelines
 * without any human input. The pipeline has already passed verifier_check
 * at registration time, so dispatch is just: acknowledge → run → EOI.
 *
 * GIC-400 on QEMU -M virt:
 * Distributor base : 0x08000000
 * CPU interface    : 0x08010000
 * All SPIs (IRQ 32+) target CPU 0 by default after irq_init().
 */

#include <kernel/irq.h>
#include <kernel/sandbox.h>
#include <kernel/mmio.h>
#include <kernel/string.h>
#include <kernel/syscall.h>
#include <stdint.h>
#include <stdbool.h>

/* ---- Dispatch table ------------------------------------------------------ */
static irq_pipeline_binding_t irq_bindings[IRQ_PIPELINE_SLOTS];
static uint32_t irq_binding_count = 0;

/* ======================================================================
 * irq_init — initialise GIC distributor and CPU interface
 * ====================================================================== */
void irq_init(void)
{
    /* Clear internal state */
    k_memset(irq_bindings, 0, sizeof(irq_bindings));
    irq_binding_count = 0;

    /* ---- Distributor (GICD) ---- */

    /* Disable distributor before configuration */
    mmio_write32(GICD_CTLR, 0U);

    /*
     * Configure SPIs (Shared Peripheral Interrupts: IRQ 32 to IRQ_MAX_SPI).
     *
     * We iterate through the interrupts and set:
     * 1. Priority (GICD_IPRIORITYR): 8 bits per IRQ, 4 IRQs per 32-bit word.
     * 2. Target (GICD_ITARGETSR): 8 bits per IRQ, 4 IRQs per 32-bit word.
     */
    for (uint32_t i = 32U; i < IRQ_MAX_SPI; i += 4U) {
        /* Set 4 interrupts at once to Priority 0xA0 (midrange) */
        mmio_write32(GICD_IPRIORITYR + (i & ~3U), 0xA0A0A0A0U);
        
        /* Set 4 interrupts at once to Target CPU 0 (0x01 per byte) */
        mmio_write32(GICD_ITARGETSR + (i & ~3U), 0x01010101U);
    }

    /* Set all SPIs to be level-sensitive (0b00 in GICD_ICFGR)
     * ICFGR handles 16 interrupts per 32-bit word (2 bits per IRQ) */
    for (uint32_t i = 32U; i < IRQ_MAX_SPI; i += 16U) {
        mmio_write32(GICD_ICFGR + (i * 4U / 16U), 0U);
    }

    /* Enable distributor */
    mmio_write32(GICD_CTLR, 1U);

    /* ---- CPU Interface (GICC) ---- */

    /* Set priority mask: allow all priorities (0xFF = permissive) */
    mmio_write32(GICC_PMR, 0xFFU);

    /* Enable CPU interface */
    mmio_write32(GICC_CTLR, 1U);
}

/* ======================================================================
 * irq_register_pipeline — bind a pipeline to a hardware IRQ number
 * ====================================================================== */
bool irq_register_pipeline(uint32_t irq_num,
                             const pipeline_t *pipeline,
                             sandbox_ctx_t *ctx)
{
    if (!pipeline || !ctx)              return false;
    if (irq_num < 32U || irq_num >= IRQ_MAX_SPI) return false;

    /* Check for duplicate binding — one pipeline per IRQ */
    for (uint32_t i = 0; i < IRQ_PIPELINE_SLOTS; i++) {
        if (irq_bindings[i].active && irq_bindings[i].irq_num == irq_num) {
            /* Update existing binding */
            irq_bindings[i].pipeline = pipeline;
            irq_bindings[i].ctx      = ctx;
            return true;
        }
    }

    /* Find first free slot */
    if (irq_binding_count >= IRQ_PIPELINE_SLOTS) return false;

    irq_bindings[irq_binding_count].irq_num  = irq_num;
    irq_bindings[irq_binding_count].pipeline = pipeline;
    irq_bindings[irq_binding_count].ctx      = ctx;
    irq_bindings[irq_binding_count].active   = true;
    irq_binding_count++;

    return true;
}

/* ======================================================================
 * irq_enable — unmask one interrupt at the GIC distributor
 * ====================================================================== */
void irq_enable(uint32_t irq_num)
{
    if (irq_num >= IRQ_MAX_SPI) return;

    /* GICD_ISENABLER is a bitfield: bit N enables IRQ N */
    uint32_t reg    = irq_num / 32U;
    uint32_t bit    = irq_num % 32U;
    mmio_write32(GICD_ISENABLER + reg * 4U, 1U << bit);
}

/* ======================================================================
 * irq_disable — mask one interrupt at the GIC distributor
 * ====================================================================== */
void irq_disable(uint32_t irq_num)
{
    if (irq_num >= IRQ_MAX_SPI) return;

    /* GICD_ICENABLER: Writing a 1 to a bit disables the IRQ */
    uint32_t reg    = irq_num / 32U;
    uint32_t bit    = irq_num % 32U;
    mmio_write32(GICD_ICENABLER + reg * 4U, 1U << bit);
}

/* ======================================================================
 * irq_enable_in_cpu — unmask IRQs at the CPU PSTATE level
 * ====================================================================== */
void irq_enable_in_cpu(void)
{
    /* Clear DAIF.I bit and synchronize the pipeline */
    __asm__ volatile (
        "msr daifclr, #2 \n\t"
        "isb"
        ::: "memory"
    );
}

/* ======================================================================
 * irq_dispatch — handle the interrupt exception
 * ====================================================================== */
void irq_dispatch(void)
{
    /* 1. Acknowledge the interrupt */
    uint32_t iar = mmio_read32(GICC_IAR);
    uint32_t irq_num = iar & 0x3FFU;   /* bits[9:0] = interrupt ID */

    /* Spurious interrupt (ID 1023) — just return */
    if (irq_num >= 1022U) return;

    /* 2. Find and execute the bound pipeline */
    for (uint32_t i = 0; i < IRQ_PIPELINE_SLOTS; i++) {
        if (irq_bindings[i].active && irq_bindings[i].irq_num == irq_num) {
            
            /* Execute the pre-verified AIQL pipeline */
            interpreter_exec_pipeline(
                (pipeline_t *)irq_bindings[i].pipeline,
                irq_bindings[i].ctx
            );
            break;
        }
    }

    /* 3. Signal End of Interrupt (EOI) */
    mmio_write32(GICC_EOIR, iar);
}