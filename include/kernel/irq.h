/*
 * include/kernel/irq.h — KAI OS interrupt controller interface
 *
 * Targets the ARM GIC-400 (Generic Interrupt Controller v2) present in
 * the QEMU -M virt machine at the following base addresses:
 *
 * 0x08000000 — GIC Distributor (GICD)
 * 0x08010000 — GIC CPU Interface (GICC)
 *
 * Interrupt-driven AIQL pipelines:
 * irq_register_pipeline() binds a pre-parsed pipeline_t to a hardware
 * interrupt number. When that IRQ fires, irq_dispatch() (called from the
 * assembly IRQ handler) runs the pipeline in the kernel sandbox context.
 * This gives the AI "reflexes" — sensor-triggered cognitive pipelines
 * that run without a human typing a command.
 *
 * IRQ numbering (GICv2 SPI = Shared Peripheral Interrupt):
 * IRQ 0–15   : SGI  (Software Generated) — not used by KAI
 * IRQ 16–31  : PPI  (Private Peripheral)  — timer, etc.
 * IRQ 32+    : SPI  (Shared Peripheral)   — GPIO, sensors, actuators
 *
 * The pipeline registered for a given IRQ number is verified before
 * registration, not at dispatch time, so interrupt latency is minimised.
 */

#ifndef KERNEL_IRQ_H
#define KERNEL_IRQ_H

#include <kernel/sandbox.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- GIC-400 base addresses (QEMU -M virt) ------------------------------ */
#define GICD_BASE       ((uintptr_t)0x08000000UL)   /* Distributor          */
#define GICC_BASE       ((uintptr_t)0x08010000UL)   /* CPU Interface        */

/* ---- GIC Distributor register offsets ----------------------------------- */
#define GICD_CTLR       (GICD_BASE + 0x000U)   /* Distributor control      */
#define GICD_ISENABLER  (GICD_BASE + 0x100U)   /* Interrupt set-enable     */
#define GICD_ICENABLER  (GICD_BASE + 0x180U)   /* Interrupt clear-enable   */
#define GICD_IPRIORITYR (GICD_BASE + 0x400U)   /* Interrupt priority       */
#define GICD_ITARGETSR  (GICD_BASE + 0x800U)   /* Interrupt target CPU     */
#define GICD_ICFGR      (GICD_BASE + 0xC00U)   /* Interrupt configuration  */

/* ---- GIC CPU Interface register offsets --------------------------------- */
#define GICC_CTLR       (GICC_BASE + 0x000U)   /* CPU interface control    */
#define GICC_PMR        (GICC_BASE + 0x004U)   /* Priority mask register   */
#define GICC_IAR        (GICC_BASE + 0x00CU)   /* Interrupt acknowledge    */
#define GICC_EOIR       (GICC_BASE + 0x010U)   /* End of interrupt         */

/* ---- Configuration ------------------------------------------------------ */
#define IRQ_PIPELINE_SLOTS   8U    /* Max IRQ-to-pipeline bindings           */
#define IRQ_MAX_SPI          96U   /* Max SPI IRQ number supported           */

/* ---- ARM Generic Timer (PPI IRQ 27) ------------------------------------- */
#define IRQ_TIMER_PPI        27U   /* EL1 physical timer PPI                 */
#define CNTP_CTL_ENABLE      (1U << 0)  /* Timer enable bit                  */
#define CNTP_CTL_IMASK       (1U << 1)  /* Interrupt mask (0=unmasked)        */
#define CNTP_CTL_ISTATUS     (1U << 2)  /* Condition met (read-only)          */

/* ---- IRQ-to-AIQL binding ------------------------------------------------ */
/*
 * Upgraded from pipeline_t* to aiql_program_t so full AIQL programs
 * (multi-pipeline, with RESPOND: output) can fire on hardware events.
 * The program is verified at bind time; dispatch just executes.
 */
#include <kernel/aiql.h>

typedef struct {
    uint32_t        irq_num;        /* GIC interrupt number                  */
    aiql_program_t  program;        /* Full AIQL program (copied at bind)    */
    sandbox_ctx_t  *ctx;            /* Sandbox context                       */
    bool            active;         /* Slot in use                           */
    uint32_t        fire_count;     /* How many times this IRQ has fired     */
} irq_pipeline_binding_t;

/* ---- Public API --------------------------------------------------------- */

/*
 * irq_init — initialise the GIC distributor and CPU interface.
 *
 * Enables the distributor, sets all interrupt priorities to a default
 * unmasked level, targets all SPIs at CPU 0, and enables the CPU
 * interface with a permissive priority mask.
 *
 * Must be called before irq_register_pipeline() or irq_enable_in_cpu().
 */
void irq_init(void);

/*
 * irq_register_pipeline — bind a pipeline to a hardware interrupt.
 *
 * irq_num  : GIC interrupt number to bind (SPI range: 32–IRQ_MAX_SPI).
 * pipeline : Pre-verified pipeline_t. The pipeline MUST have been run
 * through verifier_check_pipeline() before registration.
 * irq_register_pipeline does not re-verify.
 * ctx      : Sandbox context to use during execution. Must remain valid
 * for the lifetime of the binding.
 *
 * Returns true on success, false if the slot table is full or irq_num
 * is out of range.
 */
bool irq_register_pipeline(uint32_t irq_num,
                            const aiql_program_t *program,
                            sandbox_ctx_t *ctx);

/*
 * timer_init — configure the ARM generic timer to fire at interval_ms.
 *
 * Sets CNTP_TVAL_EL0 and enables the EL1 physical timer. Fires PPI IRQ 27.
 * Call irq_register_pipeline(IRQ_TIMER_PPI, ...) first, then timer_init().
 */
void timer_init(uint32_t interval_ms);

/*
 * timer_reload — reset the timer countdown (call from IRQ handler to repeat).
 */
void timer_reload(uint32_t interval_ms);

/*
 * irq_list — print all active bindings over UART.
 */
void irq_list(uint32_t caps);

/*
 * irq_enable — unmask a specific interrupt at the GIC distributor level.
 *
 * Must be called after irq_register_pipeline() to actually allow the IRQ
 * to fire. Enables the interrupt in GICD_ISENABLER.
 */
void irq_enable(uint32_t irq_num);

/*
 * irq_disable — mask a specific interrupt at the GIC distributor level.
 *
 * Required by sys_irq_control to allow the AI to shut down specific
 * hardware pipelines.
 */
void irq_disable(uint32_t irq_num);

/*
 * irq_enable_in_cpu — unmask interrupts at the CPU interface level.
 *
 * Enables the CPU interface (GICC_CTLR) and sets the priority mask
 * (GICC_PMR) to allow all interrupt priorities through.
 *
 * Also sets the DAIF.I bit in PSTATE to unmask IRQs at the processor
 * level. Call this after irq_init() and all irq_enable() calls to
 * begin receiving interrupts.
 */
void irq_enable_in_cpu(void);

/*
 * irq_dispatch — called from the assembly IRQ handler (irq_handler in mmu.S).
 *
 * Reads GICC_IAR to acknowledge the interrupt, looks up the registered
 * pipeline for that IRQ number, executes it, then writes GICC_EOIR.
 */
void irq_dispatch(void);

#ifdef __cplusplus
}
#endif

#endif /* KERNEL_IRQ_H */