/*
 * src/sandbox/sandbox.c — KAI OS sandbox entry point
 *
 * Public API:
 *   sandbox_init         — zero context, set capability mask
 *   sandbox_execute      — single-shot parse → verify → exec
 *   sandbox_run_pipeline — pipeline  parse → verify → exec
 *   sandbox_result_str   — human-readable result codes
 *
 * Single-shot pipeline (sandbox_execute):
 *   input → interpreter_parse → verifier_check → interpreter_exec
 *
 * Multi-step pipeline (sandbox_run_pipeline):
 *   input → interpreter_parse_pipeline → verifier_check_pipeline
 *         → interpreter_exec_pipeline
 *
 * The sandbox never writes outside ctx->scratch. All reads are
 * address-whitelisted before any byte is touched. Capability checks
 * happen at verify time, before execution begins.
 */
#include <kernel/sandbox.h>
#include <kernel/intent.h> 
#include <kernel/syscall.h>
#include <kernel/string.h>
#include <stdint.h>
#include <stddef.h>

/* ======================================================================
 * sandbox_init
 * ====================================================================== */
void sandbox_init(sandbox_ctx_t *ctx, intent_object_t *intent)
{
    if (!ctx || !intent) return;

    k_memset(ctx, 0, sizeof(sandbox_ctx_t));

    ctx->intent = intent;
    ctx->caps = intent->caps;
    ctx->instruction_budget = intent->instruction_budget;

    // clear scratch buffer
    for (size_t i = 0; i < SANDBOX_SCRATCH_SIZE; i++)
        ctx->scratch[i] = 0;

    // clear variable store
    for (size_t i = 0; i < VAR_STORE_SIZE; i++)
        ctx->vars.entries[i].set = false;
}

/* ======================================================================
 * sandbox_execute — single tool call
 * ====================================================================== */
sandbox_result_t sandbox_execute(sandbox_ctx_t *ctx, const char *input)
{
    if (!ctx || !input)      return SANDBOX_ERR_UNKNOWN;
    if (input[0] == '\0')    return SANDBOX_ERR_PARSE;

    /* Stage 1: Parse */
    ast_node_t node;
    if (!interpreter_parse(input, &node)) {
        sys_uart_write("[sandbox] parse error\n", 22, ctx->caps);
        return SANDBOX_ERR_PARSE;
    }

    /* Stage 2: Verify */
    if (!verifier_check(&node, ctx->caps)) {
        sys_uart_write("[sandbox] verification failed\n", 30, ctx->caps);
        return SANDBOX_ERR_VERIFY;
    }

    /* Stage 3: Execute */
    sandbox_result_t result = interpreter_exec(&node, ctx);

    if (result != SANDBOX_OK) {
        sys_uart_write("[sandbox] error: ", 17, ctx->caps);
        const char *msg = sandbox_result_str(result);
        sys_uart_write(msg, k_strlen(msg), ctx->caps);
        sys_uart_write("\n", 1, ctx->caps);
    }

    return result;
}

/* ======================================================================
 * sandbox_run_pipeline — multi-step pipeline
 *
 * Accepts semicolon-separated steps with optional "-> varname" bindings.
 *
 * Examples:
 *   "el -> level; echo done"
 *   "read 0x40000000 4 -> data; caps; echo finished"
 *   "if 1 == 1 -> then:1 else:1; echo true; echo false"
 * ====================================================================== */
sandbox_result_t sandbox_run_pipeline(sandbox_ctx_t *ctx, const char *input)
{
    if (!ctx || !input)   return SANDBOX_ERR_UNKNOWN;
    if (input[0] == '\0') return SANDBOX_ERR_PARSE;

    /* Stage 1: Parse pipeline */
    pipeline_t pipeline;
    if (!interpreter_parse_pipeline(input, &pipeline)) {
        sys_uart_write("[pipeline] parse error\n", 23, ctx->caps);
        return SANDBOX_ERR_PARSE;
    }

    /* Stage 2: Verify all steps before any execution */
    if (!verifier_check_pipeline(&pipeline, ctx->caps)) {
        sys_uart_write("[pipeline] verification failed\n", 31, ctx->caps);
        return SANDBOX_ERR_VERIFY;
    }

    /* Stage 3: Execute */
    sandbox_result_t result = interpreter_exec_pipeline(&pipeline, ctx);

    if (result != SANDBOX_OK) {
        sys_uart_write("[pipeline] error: ", 18, ctx->caps);
        const char *msg = sandbox_result_str(result);
        sys_uart_write(msg, k_strlen(msg), ctx->caps);
        sys_uart_write("\n", 1, ctx->caps);
    }

    return result;
}

/* ======================================================================
 * sandbox_result_str
 * ====================================================================== */
const char *sandbox_result_str(sandbox_result_t result)
{
    switch (result) {
        case SANDBOX_OK:           return "ok";
        case SANDBOX_ERR_DENIED:   return "permission denied";
        case SANDBOX_ERR_PARSE:    return "parse error";
        case SANDBOX_ERR_VERIFY:   return "verification failed";
        case SANDBOX_ERR_FAULT:    return "memory fault";
        case SANDBOX_ERR_LIMIT:    return "instruction limit exceeded";
        case SANDBOX_ERR_UNKNOWN:  return "unknown error";
        case SANDBOX_ERR_VAR:      return "variable error";
        case SANDBOX_ERR_COND:     return "condition error";
        case SANDBOX_ERR_EL0:      return "EL0 execution fault";
        default:                   return "unknown error";
    }
}