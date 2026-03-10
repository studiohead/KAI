/*
 * include/kernel/sandbox.h — KAI OS lightweight AI tool sandbox
 *
 * This version extends the original single-shot sandbox with pipeline
 * execution derived from the AIQL/PIQL AST schema:
 *
 *   PipelineStatement   → pipeline_t (array of pipeline_node_t steps)
 *   Operation           → pipeline_node_t with output_var binding
 *   ConditionalStatement→ OP_IF with then/else step index ranges
 *   Variable            → var_entry_t looked up in var_store_t
 *   Literal             → uint64_t value inline in pipeline_node_t
 *   BinaryExpression    → pipeline_cond_t (left op right)
 *
 * Dropped from AIQL (not portable to bare metal):
 *   CallStatement (model/visualize) — requires network / display
 *   LoadStatement                   — requires filesystem / network
 *
 * Single tool call format (unchanged):
 *   <opcode> [arg0] [arg1]
 *   e.g. "read 0x40001000 16"
 *
 * Pipeline text format (new):
 *   Steps separated by semicolons, output binding with ->
 *   e.g. "el -> level; echo done"
 *        "read 0x40000000 4 -> result; caps"
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
#define SANDBOX_SCRATCH_SIZE     256U   /* Isolated writable scratch buffer          */
#define SANDBOX_MAX_ARGS         4U     /* Max operands per node                     */
#define SANDBOX_MAX_INSNS        64U    /* Hard instruction limit (covers pipelines) */
#define SANDBOX_ARG_MAX_LEN      32U    /* Max length of a single argument string    */
#define PIPELINE_MAX_STEPS       16U    /* Max steps in one pipeline                 */
#define VAR_STORE_SIZE           8U     /* Max named variables per pipeline run      */

/* ---- Opcodes ------------------------------------------------------------- */
typedef enum {
    OP_NOP     = 0,   /* No operation                                  */
    OP_READ    = 1,   /* Read bytes from a whitelisted memory address  */
    OP_WRITE   = 2,   /* Write a byte to the scratch buffer            */
    OP_INFO    = 3,   /* Print BSS / stack memory layout info          */
    OP_ECHO    = 4,   /* Write a string to UART                        */
    OP_EL      = 5,   /* Print current exception level                 */
    OP_CAPS    = 6,   /* Print current session capability mask         */
    OP_IF         = 7,  /* Conditional branch (AIQL ConditionalStatement) */
    OP_SLEEP      = 8,  /* Busy-wait N milliseconds — robot timing/PWM    */
    OP_INTROSPECT = 9,  /* Print whitelisted MMIO map as name:address pairs*/
    OP_WAIT_EVENT = 10, /* Yield until next IRQ tick (stub; becomes async) */
    OP_RESPOND    = 11, /* Emit structured RESPOND:{...} JSON result packet */
    OP_INVALID    = 12, /* Sentinel — any value >= OP_INVALID is illegal   */
} sandbox_opcode_t;

/* ---- Comparison operators (BinaryExpression operators from AIQL) -------- */
typedef enum {
    CMP_EQ  = 0,   /* == */
    CMP_NEQ = 1,   /* != */
    CMP_LT  = 2,   /* <  */
    CMP_GT  = 3,   /* >  */
    CMP_LTE = 4,   /* <= */
    CMP_GTE = 5,   /* >= */
} cmp_op_t;

/* ---- Operand (Variable or Literal from AIQL) ---------------------------- */
typedef enum {
    OPERAND_LITERAL  = 0,   /* Inline uint64_t value        */
    OPERAND_VARIABLE = 1,   /* Named variable lookup        */
} operand_kind_t;

typedef struct {
    operand_kind_t kind;
    uint64_t       literal;                       /* kind == OPERAND_LITERAL  */
    char           var_name[SANDBOX_ARG_MAX_LEN]; /* kind == OPERAND_VARIABLE */
} operand_t;

/* ---- Condition (maps to AIQL BinaryExpression) -------------------------- */
typedef struct {
    operand_t left;
    cmp_op_t  op;
    operand_t right;
} pipeline_cond_t;

/* ---- Pipeline node (maps to AIQL Operation / ConditionalStatement) ------ */
/*
 * output_var: if non-empty, the numeric result of this step is stored
 * in the variable store under this name for use by later steps.
 *
 * For OP_IF:
 *   cond       — condition to evaluate (BinaryExpression)
 *   then_count — number of following steps to execute if true
 *   else_count — number of steps after then_count to run if false
 *                (0 = no else branch)
 */
typedef struct {
    sandbox_opcode_t opcode;
    uint32_t         argc;
    char             args[SANDBOX_MAX_ARGS][SANDBOX_ARG_MAX_LEN];
    char             output_var[SANDBOX_ARG_MAX_LEN]; /* "" = no binding     */
    pipeline_cond_t  cond;        /* OP_IF only                              */
    uint32_t         then_count;  /* OP_IF only                              */
    uint32_t         else_count;  /* OP_IF only                              */
} pipeline_node_t;

/* ---- Variable store (maps to AIQL Variable + named outputs) ------------- */
typedef struct {
    char     name[SANDBOX_ARG_MAX_LEN];
    uint64_t value;
    bool     set;
} var_entry_t;

typedef struct {
    var_entry_t entries[VAR_STORE_SIZE];
} var_store_t;

/* ---- Pipeline (maps to AIQL PipelineStatement) -------------------------- */
/* pipeline_t fully defined here */
typedef struct {
    pipeline_node_t steps[PIPELINE_MAX_STEPS];
    uint32_t        step_count;
} pipeline_t;

/* ---- Single AST node (unchanged — used by single-shot sandbox_execute) -- */
typedef struct {
    sandbox_opcode_t opcode;
    uint32_t         argc;
    char             args[SANDBOX_MAX_ARGS][SANDBOX_ARG_MAX_LEN];
} ast_node_t;

/* ---- Result codes -------------------------------------------------------- */
typedef enum {
    SANDBOX_OK            = 0,
    SANDBOX_ERR_DENIED    = 1,   /* Capability check failed          */
    SANDBOX_ERR_PARSE     = 2,   /* Input could not be parsed        */
    SANDBOX_ERR_VERIFY    = 3,   /* Verifier rejected the input      */
    SANDBOX_ERR_FAULT     = 4,   /* Memory access violation          */
    SANDBOX_ERR_LIMIT     = 5,   /* Instruction count exceeded       */
    SANDBOX_ERR_UNKNOWN   = 6,   /* Unrecognised opcode              */
    SANDBOX_ERR_VAR       = 7,   /* Variable not found or store full */
    SANDBOX_ERR_COND      = 8,   /* Condition evaluation failed      */
    SANDBOX_ERR_EL0       = 9,   /* EL0 execution fault              */
} sandbox_result_t;

/* ---- Sandbox context ----------------------------------------------------- */
/* Need intent_object_t pointer */
#include "intent.h"
typedef struct intent_object intent_object_t;

typedef struct {

    /* ---- Intent binding ---- */
    intent_object_t *intent;

    /* ---- Capability isolation ---- */
    uint32_t caps;

    /* ---- Hard execution limits ---- */
    uint32_t instruction_budget;
    uint32_t instruction_count;

    /* ---- Private isolated memory ---- */
    uint8_t scratch[SANDBOX_SCRATCH_SIZE];
    size_t  scratch_used;

    /* ---- Intent-local variables ---- */
    var_store_t vars;

} sandbox_ctx_t;

/* ---- Public API ---------------------------------------------------------- */

/* Initialise context. Must be called before any execute/run call. */
void sandbox_init(sandbox_ctx_t *ctx, intent_object_t *intent);

/* Single-shot: parse one tool call, verify, execute. */
sandbox_result_t sandbox_execute(sandbox_ctx_t *ctx, const char *input);

/*
 * Pipeline: parse semicolon-separated steps, verify all, execute in sequence.
 * Supports "-> varname" output bindings and OP_IF conditional branches.
 * e.g.  "el -> level; echo pipeline done"
 *        "read 0x40000000 4 -> data; caps"
 */
sandbox_result_t sandbox_run_pipeline(sandbox_ctx_t *ctx, const char *input);

/* Human-readable result string. Never returns NULL. */
const char *sandbox_result_str(sandbox_result_t result);

/* ---- Variable store helpers (exposed for testing) ----------------------- */
bool var_store_set(var_store_t *store, const char *name, uint64_t value);
bool var_store_get(const var_store_t *store, const char *name, uint64_t *out);

/* ---- Verifier (exposed for testing) ------------------------------------- */
bool verifier_check(const ast_node_t *node, uint32_t caps);
bool verifier_check_pipeline(const pipeline_t *pipeline, uint32_t caps);

/* ---- Interpreter (used internally by sandbox.c) ------------------------- */
bool             interpreter_parse(const char *input, ast_node_t *out_node);
bool             interpreter_parse_pipeline(const char *input, pipeline_t *out_pipeline);
sandbox_result_t interpreter_exec(ast_node_t *node, sandbox_ctx_t *ctx);
sandbox_result_t interpreter_exec_pipeline(pipeline_t *pipeline, sandbox_ctx_t *ctx);

#ifdef __cplusplus
}
#endif

#endif /* KERNEL_SANDBOX_H */