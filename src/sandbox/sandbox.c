/*
 * src/sandbox/sandbox.c — KAI OS sandbox entry point
 *
 * Provides the public-facing sandbox API:
 *   - sandbox_init    : initialise a context with a capability mask
 *   - sandbox_execute : parse → verify → execute a tool_call string
 *   - sandbox_result_str : human-readable result codes
 *
 * Execution pipeline for sandbox_execute:
 *
 *   input string
 *       │
 *       ▼
 *   interpreter_parse()   — tokenise into ast_node_t
 *       │  failure → SANDBOX_ERR_PARSE
 *       ▼
 *   verifier_check()      — validate opcodes, args, caps, addresses
 *       │  failure → SANDBOX_ERR_VERIFY
 *       ▼
 *   interpreter_exec()    — execute within sandbox_ctx_t
 *       │
 *       ▼
 *   sandbox_result_t      — returned to caller
 *
 * The sandbox never touches kernel .data or .bss directly.
 * All writes land in ctx->scratch. All reads are whitelisted by
 * the verifier + sys_mem_read before any byte is accessed.
 */

#include <kernel/sandbox.h>
#include <kernel/syscall.h>
#include <kernel/string.h>
#include <stdint.h>
#include <stddef.h>

/* ======================================================================
 * sandbox_init
 *
 * Zero-initialises the context and sets the capability mask.
 * scratch buffer is explicitly zeroed so no stale data leaks between
 * sandbox invocations.
 * ====================================================================== */
void sandbox_init(sandbox_ctx_t *ctx, uint32_t caps)
{
    if (!ctx)
        return;

    k_memset(ctx, 0, sizeof(sandbox_ctx_t));
    ctx->caps        = caps;
    ctx->insn_count  = 0U;
    ctx->scratch_used = 0U;
}

/* ======================================================================
 * sandbox_execute
 *
 * Full pipeline: parse → verify → execute.
 * Returns SANDBOX_OK on success or a specific error code on failure.
 * ====================================================================== */
sandbox_result_t sandbox_execute(sandbox_ctx_t *ctx, const char *input)
{
    if (!ctx || !input)
        return SANDBOX_ERR_UNKNOWN;

    /* Reject empty input early */
    if (input[0] == '\0')
        return SANDBOX_ERR_PARSE;

    /* ---- Stage 1: Parse ---------------------------------------------- */
    ast_node_t node;
    if (!interpreter_parse(input, &node)) {
        sys_uart_write("[sandbox] parse error\n",
                       22, ctx->caps);
        return SANDBOX_ERR_PARSE;
    }

    /* ---- Stage 2: Verify --------------------------------------------- */
    if (!verifier_check(&node, ctx->caps)) {
        sys_uart_write("[sandbox] verification failed\n",
                       30, ctx->caps);
        return SANDBOX_ERR_VERIFY;
    }

    /* ---- Stage 3: Execute -------------------------------------------- */
    sandbox_result_t result = interpreter_exec(&node, ctx);

    if (result != SANDBOX_OK) {
        sys_uart_write("[sandbox] execution error: ", 27, ctx->caps);
        sys_uart_write(sandbox_result_str(result),
                       k_strlen(sandbox_result_str(result)),
                       ctx->caps);
        sys_uart_write("\n", 1, ctx->caps);
    }

    return result;
}

/* ======================================================================
 * sandbox_result_str
 *
 * Maps a sandbox_result_t to a human-readable string.
 * Always returns a valid NUL-terminated string, never NULL.
 * ====================================================================== */
const char *sandbox_result_str(sandbox_result_t result)
{
    switch (result) {
        case SANDBOX_OK:          return "ok";
        case SANDBOX_ERR_DENIED:  return "permission denied";
        case SANDBOX_ERR_PARSE:   return "parse error";
        case SANDBOX_ERR_VERIFY:  return "verification failed";
        case SANDBOX_ERR_FAULT:   return "memory fault";
        case SANDBOX_ERR_LIMIT:   return "instruction limit exceeded";
        case SANDBOX_ERR_UNKNOWN: return "unknown error";
        default:                  return "unknown error";
    }
}