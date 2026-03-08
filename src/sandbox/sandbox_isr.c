/*
 * src/sandbox/sandbox_isr.c — KAI OS interrupt-driven pipeline engine
 *
 * Implements the GIC-400 driver and the IRQ → pipeline dispatch table.
 *
 * Interrupt-driven pipelines give the AI "reflexes": sensor events fire
 * hardware interrupts which immediately execute pre-verified AIQL pipelines
 * without any human input. The pipeline has already passed verifier_check
 * at registration time, so dispatch is just: acknowledge → run → EOI.
 *
 * GIC-400 on QEMU -M virt:
 *   Distributor base : 0x08000000
 *   CPU interface    : 0x08010000
 *   All SPIs (IRQ 32+) target CPU 0 by default after irq_init().
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
    /* ---- Distributor ---- */

    /* Disable distributor before configuration */
    mmio_write32(GICD_CTLR, 0U);

    /*
     * Set all SPIs (IRQ 32–IRQ_MAX_SPI) to:
     *   Priority 0xA0 — midrange, below the CPU interface priority mask
     *   Target CPU 0  — route to core 0
     *   Level-sensitive configuration
     *
     * GIC registers are word-wide but pack 4 priority values per word
     * (IPRIORITYR) and 4 target values per word (ITARGETSR).
     */
    for (uint32_t i = 32U; i < IRQ_MAX_SPI; i += 4U) {
        mmio_write32(GICD_IPRIORITYR + (i & ~3U),
                     0xA0A0A0A0U); /* 4 × priority 0xA0 */
        mmio_write32(GICD_ITARGETSR + (i & ~3U),
                     0x01010101U); /* 4 × CPU 0         */
    }

    /* Enable distributor */
    mmio_write32(GICD_CTLR, 1U);

    /* ---- CPU Interface ---- */

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
    if (irq_binding_count >= IRQ_PIPELINE_SLOTS)  return false;

    /* Check for duplicate binding — one pipeline per IRQ */
    for (uint32_t i = 0; i < irq_binding_count; i++) {
        if (irq_bindings[i].active &&
            irq_bindings[i].irq_num == irq_num) {
            return false; /* already registered */
        }
    }

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
 * irq_enable_in_cpu — unmask IRQs at the CPU PSTATE level
 * ====================================================================== */
void irq_enable_in_cpu(void)
{
    /* Clear DAIF.I to unmask IRQ exceptions at the processor */
    __asm__ volatile ("msr daifclr, #2" ::: "memory");
}

/* ======================================================================
 * irq_dispatch — called from assembly IRQ handler
 *
 * Acknowledge → find binding → execute pipeline → EOI.
 * Must be fast: no blocking I/O, no uart_getc.
 * ====================================================================== */
void irq_dispatch(void)
{
    /* Read and acknowledge the interrupt */
    uint32_t iar = mmio_read32(GICC_IAR);
    uint32_t irq_num = iar & 0x3FFU;   /* bits[9:0] = interrupt ID */

    /* Spurious interrupt (ID 1023) — just return */
    if (irq_num == 1023U) return;

    /* Find the registered pipeline for this IRQ */
    for (uint32_t i = 0; i < irq_binding_count; i++) {
        if (irq_bindings[i].active &&
            irq_bindings[i].irq_num == irq_num) {

            /*
             * Execute the pre-verified pipeline.
             * We call interpreter_exec_pipeline directly (bypass the
             * parse/verify stages — they already ran at registration time).
             * Cast away const: interpreter does not modify the pipeline,
             * but the function signature takes a non-const pointer for
             * the step cursor during execution.
             */
            interpreter_exec_pipeline(
                (pipeline_t *)irq_bindings[i].pipeline,
                irq_bindings[i].ctx
            );
            break;
        }
    }

    /* Signal End of Interrupt to the GIC CPU interface */
    mmio_write32(GICC_EOIR, iar);
}
