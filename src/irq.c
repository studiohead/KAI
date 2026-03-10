/*
 * src/irq.c — KAI OS interrupt controller + timer + AIQL reflex engine
 *
 * Hardware:
 *   GIC-400 distributor : 0x08000000
 *   GIC CPU interface   : 0x08010000
 *   ARM generic timer   : system registers (CNTP_*)
 *   Timer PPI           : IRQ 27 (EL1 physical timer)
 *
 * IRQ-to-AIQL bindings:
 *   irq_register_pipeline() stores a full aiql_program_t per IRQ slot.
 *   irq_dispatch() calls aiql_execute_program() — the complete AIQL path
 *   including RESPOND: packet output — directly from the interrupt handler.
 *
 * Timer reflexes:
 *   timer_init(ms) arms the EL1 physical timer to fire every ms milliseconds.
 *   It auto-reloads in irq_dispatch() so the reflex repeats indefinitely.
 *   The interval is stored globally so reload doesn't need a parameter.
 */

#include <kernel/irq.h>
#include <kernel/aiql.h>
#include <kernel/sandbox.h>
#include <kernel/mmio.h>
#include <kernel/string.h>
#include <kernel/syscall.h>
#include <kernel/uart.h>
#include <stdint.h>
#include <stdbool.h>

/* ---- Dispatch table ------------------------------------------------------ */
static irq_pipeline_binding_t irq_bindings[IRQ_PIPELINE_SLOTS];
static uint32_t irq_binding_count = 0;

/* ---- Timer state --------------------------------------------------------- */
static uint32_t timer_interval_ms = 0;   /* 0 = not configured */

/* ======================================================================
 * irq_init — initialise GIC distributor and CPU interface
 * ====================================================================== */
void irq_init(void)
{
    k_memset(irq_bindings, 0, sizeof(irq_bindings));
    irq_binding_count = 0;
    timer_interval_ms = 0;

    /* ---- Distributor (GICD) ---- */
    mmio_write32(GICD_CTLR, 0U);   /* disable before config */

    for (uint32_t i = 32U; i < IRQ_MAX_SPI; i += 4U) {
        mmio_write32(GICD_IPRIORITYR + (i & ~3U), 0xA0A0A0A0U);
        mmio_write32(GICD_ITARGETSR  + (i & ~3U), 0x01010101U);
    }
    for (uint32_t i = 32U; i < IRQ_MAX_SPI; i += 16U)
        mmio_write32(GICD_ICFGR + (i * 4U / 16U), 0U);

    mmio_write32(GICD_CTLR, 1U);   /* enable */

    /* ---- CPU Interface (GICC) ---- */
    mmio_write32(GICC_PMR,  0xFFU);  /* allow all priorities */
    mmio_write32(GICC_CTLR, 1U);     /* enable */

    /* ---- Enable PPI 27 (timer) in GICD_ISENABLER[0] bit 27 ----------- */
    /* PPIs live in GICD_ISENABLER[0], bits 16-31 */
    mmio_write32(GICD_ISENABLER, (1U << IRQ_TIMER_PPI));
}

/* ======================================================================
 * timer_init — arm the EL1 physical timer
 * ====================================================================== */
void timer_init(uint32_t interval_ms)
{
    timer_interval_ms = interval_ms;
    timer_reload(interval_ms);
}

void timer_reload(uint32_t interval_ms)
{
    uint64_t freq;
    __asm__ volatile ("mrs %0, cntfrq_el0" : "=r" (freq));
    uint64_t ticks = (freq * (uint64_t)interval_ms) / 1000ULL;

    /* Write countdown value and enable with interrupt unmasked */
    __asm__ volatile ("msr cntp_tval_el0, %0" :: "r" (ticks));
    __asm__ volatile ("msr cntp_ctl_el0,  %0" :: "r" ((uint64_t)(CNTP_CTL_ENABLE)));
    __asm__ volatile ("isb");
}

/* ======================================================================
 * irq_register_pipeline — bind an AIQL program to a hardware IRQ
 * ====================================================================== */
bool irq_register_pipeline(uint32_t irq_num,
                             const aiql_program_t *program,
                             sandbox_ctx_t *ctx)
{
    if (!program || !ctx) return false;

    /* PPI 27 (timer) is allowed; SPIs must be in range */
    if (irq_num != IRQ_TIMER_PPI && (irq_num < 32U || irq_num >= IRQ_MAX_SPI))
        return false;

    /* Update existing binding */
    for (uint32_t i = 0; i < IRQ_PIPELINE_SLOTS; i++) {
        if (irq_bindings[i].active && irq_bindings[i].irq_num == irq_num) {
            irq_bindings[i].program    = *program;   /* copy */
            irq_bindings[i].ctx        = ctx;
            irq_bindings[i].fire_count = 0;
            return true;
        }
    }

    /* New slot */
    if (irq_binding_count >= IRQ_PIPELINE_SLOTS) return false;
    irq_bindings[irq_binding_count].irq_num    = irq_num;
    irq_bindings[irq_binding_count].program    = *program;  /* copy */
    irq_bindings[irq_binding_count].ctx        = ctx;
    irq_bindings[irq_binding_count].active     = true;
    irq_bindings[irq_binding_count].fire_count = 0;
    irq_binding_count++;
    return true;
}

/* ======================================================================
 * irq_enable / irq_disable
 * ====================================================================== */
void irq_enable(uint32_t irq_num)
{
    if (irq_num >= IRQ_MAX_SPI && irq_num != IRQ_TIMER_PPI) return;
    uint32_t reg = irq_num / 32U;
    uint32_t bit = irq_num % 32U;
    mmio_write32(GICD_ISENABLER + reg * 4U, 1U << bit);
}

void irq_disable(uint32_t irq_num)
{
    if (irq_num >= IRQ_MAX_SPI && irq_num != IRQ_TIMER_PPI) return;
    uint32_t reg = irq_num / 32U;
    uint32_t bit = irq_num % 32U;
    mmio_write32(GICD_ICENABLER + reg * 4U, 1U << bit);
}

/* ======================================================================
 * irq_enable_in_cpu — unmask IRQs at PSTATE level
 * ====================================================================== */
void irq_enable_in_cpu(void)
{
    __asm__ volatile ("msr daifclr, #2\n\t isb" ::: "memory");
}

/* ======================================================================
 * irq_list — print all active bindings
 * ====================================================================== */
void irq_list(uint32_t caps)
{
    sys_uart_write("[irq] bindings: ", 16, caps);
    sys_uart_hex64((uint64_t)irq_binding_count, caps);
    sys_uart_write("\r\n", 2, caps);

    for (uint32_t i = 0; i < IRQ_PIPELINE_SLOTS; i++) {
        if (!irq_bindings[i].active) continue;
        sys_uart_write("  irq=", 6, caps);
        sys_uart_hex64((uint64_t)irq_bindings[i].irq_num, caps);
        sys_uart_write(" fires=", 7, caps);
        sys_uart_hex64((uint64_t)irq_bindings[i].fire_count, caps);
        sys_uart_write(" goal=", 6, caps);
        const char *goal = irq_bindings[i].program.goal;
        sys_uart_write(goal[0] ? goal : "(none)", goal[0] ? k_strlen(goal) : 6, caps);
        sys_uart_write("\r\n", 2, caps);
    }

    if (timer_interval_ms > 0) {
        sys_uart_write("[irq] timer interval: ", 22, caps);
        sys_uart_hex64((uint64_t)timer_interval_ms, caps);
        sys_uart_write("ms\r\n", 4, caps);
    }
}

/* ======================================================================
 * irq_dispatch — called from assembly IRQ handler
 * ====================================================================== */
void irq_dispatch(void)
{
    /* 1. Acknowledge */
    uint32_t iar     = mmio_read32(GICC_IAR);
    uint32_t irq_num = iar & 0x3FFU;

    if (irq_num >= 1022U) return;   /* spurious */

    /* 2. Find and execute bound AIQL program */
    for (uint32_t i = 0; i < IRQ_PIPELINE_SLOTS; i++) {
        if (!irq_bindings[i].active) continue;
        if (irq_bindings[i].irq_num != irq_num) continue;

        irq_bindings[i].fire_count++;

        /* Full AIQL execute path: verify already done at bind time */
        aiql_execute_program(&irq_bindings[i].program,
                             irq_bindings[i].ctx,
                             irq_bindings[i].ctx->caps);

        /* Reload timer so it fires again */
        if (irq_num == IRQ_TIMER_PPI && timer_interval_ms > 0)
            timer_reload(timer_interval_ms);

        break;
    }

    /* 3. EOI */
    mmio_write32(GICC_EOIR, iar);
}