/*
 * include/kernel/intent.h — KAI OS AI intent object
 *
 * Defines the intent_object_t type, which represents a verified,
 * capability- and instruction-limited intent that governs sandbox execution.
 *
 * Any sandbox or kernel code that needs to read/write intent fields
 * must include this header.
 */

#ifndef KERNEL_INTENT_H
#define KERNEL_INTENT_H

#include <stdint.h>

/* ----------------------------------------------------------------------
 * AI intent object
 *
 * Defines the execution context and limits for a verified AI instruction
 * or pipeline. The sandbox derives capabilities and instruction budget
 * directly from this object.
 * -------------------------------------------------------------------- */
typedef struct intent_object {
    uint32_t caps;                   /* capability bitmask (CAP_READ_MEM, etc.) */
    uint32_t instruction_budget;     /* max allowed instructions/pipeline steps */
    const pipeline_t *pipeline;      /* optional preloaded pipeline (read-only) */
} intent_object_t;

#endif /* KERNEL_INTENT_H */