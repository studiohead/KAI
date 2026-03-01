/*
 * include/kernel/sandbox.h — KAI OS lightweight AI tool sandbox
 *
 * Defines the AST node types, opcode set, sandbox session context,
 * and public interface for the verifier, interpreter, and sandbox
 * entry point.
 *
 * Design goals:
 *   - Parse AI tool_call text into a restricted AST
 *   - Verify the AST before any execution (whitelist opcodes, operands)
 *   - Execute in a capability-gated context with a hard instruction limit
 *   - Trap violations via the existing exception/capability infrastructure
 *
 * Tool call input format (simple text):
 *   <command> [arg1] [arg2]
 *   e.g. "read 0x40001000 16"
 *        "write 0x40002000 0xFF"
 *        "info"
 *        "echo hello world"
 */

#ifndef KERNEL_SANDBOX_H
#define KERNEL_SANDBOX_H

#include <kernel/syscall.h>
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Configuration ------------------------------------------------------- */
#define SANDBOX_SCRATCH_SIZE    256U    /* Bytes in the isolated scratch buffer */
#define SANDBOX_MAX_ARGS        4U      /* Max operands per AST node            */
#define SANDBOX_MAX_INSNS       32U     /* Hard instruction count limit         */
#define SANDBOX_ARG_MAX_LEN     32U     /* Max length of a single argument      */

/* ---- Opcodes ------------------------------------------------------------- */
/*
 * Each tool_call maps to exactly one opcode. The verifier rejects any
 * opcode not in this list before execution begins.
 */
typedef enum {
    OP_NOP    = 0,   /* No operation — safe default / padding        */
    OP_READ   = 1,   /* Read bytes from a whitelisted memory address  */
    OP_WRITE  = 2,   /* Write a byte to the scratch buffer only       */
    OP_INFO   = 3,   /* Print BSS / stack memory layout info          */
    OP_ECHO   = 4,   /* Write a string to UART                        */
    OP_EL     = 5,   /* Print current exception level                 */
    OP_CAPS   = 6,   /* Print current session capability mask         */
    OP_INVALID = 7,  /* Sentinel — any value >= OP_INVALID is illegal */
} sandbox_opcode_t;

/* ---- AST node ------------------------------------------------------------ */
/*
 * A parsed tool_call is represented as a single flat AST node.
 * Complex chaining is intentionally unsupported — one call, one node.
 * args[] holds raw NUL-terminated argument strings parsed from input.
 */
typedef struct {
    sandbox_opcode_t opcode;
    uint32_t         argc;
    char             args[SANDBOX_MAX_ARGS][SANDBOX_ARG_MAX_LEN];
} ast_node_t;

/* ---- Sandbox result ------------------------------------------------------ */
typedef enum {
    SANDBOX_OK            = 0,   /* Execution completed successfully      */
    SANDBOX_ERR_DENIED    = 1,   /* Capability check failed               */
    SANDBOX_ERR_PARSE     = 2,   /* Input could not be parsed             */
    SANDBOX_ERR_VERIFY    = 3,   /* Verifier rejected the AST node        */
    SANDBOX_ERR_FAULT     = 4,   /* Memory access violation during exec   */
    SANDBOX_ERR_LIMIT     = 5,   /* Instruction count limit exceeded      */
    SANDBOX_ERR_UNKNOWN   = 6,   /* Unrecognised opcode at execution time  */
} sandbox_result_t;

/* ---- Sandbox context ----------------------------------------------------- */
/*
 * Holds all state for a single sandbox execution.
 * The scratch buffer is the only writable memory target for OP_WRITE —
 * sandboxed code can never write outside this region.
 */
typedef struct {
    uint32_t  caps;                          /* Capability mask for this session   */
    uint32_t  insn_count;                    /* Instructions executed so far       */
    uint8_t   scratch[SANDBOX_SCRATCH_SIZE]; /* Isolated writable scratch region   */
    size_t    scratch_used;                  /* Bytes written into scratch so far  */
} sandbox_ctx_t;

/* ---- Public interface ---------------------------------------------------- */

/*
 * sandbox_init — initialise a sandbox context with a capability mask.
 * Must be called before sandbox_execute.
 */
void sandbox_init(sandbox_ctx_t *ctx, uint32_t caps);

/*
 * sandbox_execute — parse, verify, and execute a single tool_call string.
 *
 * Parameters:
 *   ctx   : Initialised sandbox context
 *   input : NUL-terminated tool_call string (e.g. "read 0x40001000 16")
 *
 * Returns a sandbox_result_t indicating success or the failure reason.
 * On any error the scratch buffer and UART output are left in whatever
 * partial state they reached before the fault — the kernel is not harmed.
 */
sandbox_result_t sandbox_execute(sandbox_ctx_t *ctx, const char *input);

/*
 * sandbox_result_str — return a human-readable string for a result code.
 * Always returns a valid NUL-terminated string, never NULL.
 */
const char *sandbox_result_str(sandbox_result_t result);

/* ---- Verifier (used internally by sandbox.c, exposed for testing) -------- */

/*
 * verifier_check — validate an AST node before execution.
 * Returns true if the node is safe to execute, false otherwise.
 */
bool verifier_check(const ast_node_t *node, uint32_t caps);

/* ---- Interpreter (used internally by sandbox.c) -------------------------- */

/*
 * interpreter_parse — parse a raw input string into an AST node.
 * Returns true on success, false if the input is malformed.
 */
bool interpreter_parse(const char *input, ast_node_t *out_node);

/*
 * interpreter_exec — execute a verified AST node within a sandbox context.
 * Caller must run verifier_check first — this function does not re-verify.
 */
sandbox_result_t interpreter_exec(ast_node_t *node, sandbox_ctx_t *ctx);

#ifdef __cplusplus
}
#endif

#endif /* KERNEL_SANDBOX_H */