/*
 * include/kernel/aiql.h — Kernel-side AIQL JSON extractor + executor
 *
 * Walks a known AIQL JSON schema shape and builds pipeline_t structs
 * directly — no general-purpose parser needed.
 *
 * The shell `aiql` command reads a large JSON buffer (up to AIQL_BUF_SIZE)
 * then calls aiql_extract() + aiql_execute_program() in one shot.
 */

#ifndef KERNEL_AIQL_H
#define KERNEL_AIQL_H

#include <kernel/sandbox.h>
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* Max JSON the kernel will accept in one aiql command */
#define AIQL_BUF_SIZE     4096U
/* Max PipelineStatements extracted from one Program */
#define AIQL_MAX_PIPELINES   8U

typedef enum {
    AIQL_OK           = 0,
    AIQL_ERR_PARSE    = 1,   /* schema not recognised     */
    AIQL_ERR_NOBODY   = 2,   /* no executable body        */
    AIQL_ERR_OVERFLOW = 3,   /* too many pipelines/steps  */
    AIQL_ERR_NULL     = 4,   /* null argument             */
} aiql_err_t;

typedef struct {
    char       goal[64];
    pipeline_t pipelines[AIQL_MAX_PIPELINES];
    uint32_t   pipeline_count;
} aiql_program_t;

/*
 * aiql_extract  — parse JSON into aiql_program_t.
 * Returns AIQL_OK on success; program->pipeline_count >= 1.
 */
aiql_err_t aiql_extract(const char *json, size_t len, aiql_program_t *out);

/*
 * aiql_execute_program — verify + run every pipeline in order.
 * Prints goal, per-pipeline output, errors.
 */
sandbox_result_t aiql_execute_program(aiql_program_t *prog,
                                      sandbox_ctx_t  *ctx,
                                      uint32_t        caps);

const char *aiql_err_str(aiql_err_t e);

#endif /* KERNEL_AIQL_H */