/*
 * src/sandbox/interpreter.c — KAI OS sandbox interpreter
 *
 * Handles both single-shot and pipeline execution:
 *
 *   interpreter_parse          — tokenise one tool call into ast_node_t
 *   interpreter_exec           — execute one verified ast_node_t
 *   interpreter_parse_pipeline — parse semicolon-separated steps into pipeline_t
 *                                Handles "-> varname" output bindings
 *                                Handles "if <cond> { ... } else { ... }" blocks
 *   interpreter_exec_pipeline  — execute a verified pipeline_t step by step
 *                                with variable store, OP_IF branching, and
 *                                instruction count across the whole pipeline
 *
 * Variable store helpers:
 *   var_store_set — bind a name to a uint64_t value
 *   var_store_get — look up a name, return false if not found
 *
 * The interpreter never re-verifies. Callers must run verifier_check /
 * verifier_check_pipeline before calling exec functions.
 */

#include <kernel/sandbox.h>
#include <kernel/syscall.h>
#include <kernel/memory.h>
#include <kernel/string.h>
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* ---- Opcode name lookup table ------------------------------------------ */
typedef struct {
    const char       *name;
    sandbox_opcode_t  opcode;
} opcode_entry_t;

static const opcode_entry_t opcode_table[] = {
    { "nop",   OP_NOP   },
    { "read",  OP_READ  },
    { "write", OP_WRITE },
    { "info",  OP_INFO  },
    { "echo",  OP_ECHO  },
    { "el",    OP_EL    },
    { "caps",  OP_CAPS  },
    { "if",    OP_IF    },
};

#define OPCODE_TABLE_SIZE (sizeof(opcode_table) / sizeof(opcode_table[0]))

/* ---- Internal string helpers ------------------------------------------- */

static void safe_strcpy(char *dst, const char *src, size_t max)
{
    size_t i = 0;
    while (i < max - 1U && src[i] != '\0') { dst[i] = src[i]; i++; }
    dst[i] = '\0';
}

static bool parse_uint64(const char *s, uint64_t *out)
{
    if (!s || s[0] == '\0') return false;
    uint64_t result = 0;
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        s += 2;
        if (s[0] == '\0') return false;
        while (*s) {
            uint8_t n;
            if      (*s >= '0' && *s <= '9') n = (uint8_t)(*s - '0');
            else if (*s >= 'a' && *s <= 'f') n = (uint8_t)(*s - 'a' + 10);
            else if (*s >= 'A' && *s <= 'F') n = (uint8_t)(*s - 'A' + 10);
            else return false;
            result = (result << 4) | n;
            s++;
        }
    } else {
        while (*s) {
            if (*s < '0' || *s > '9') return false;
            result = result * 10U + (uint64_t)(*s - '0');
            s++;
        }
    }
    *out = result;
    return true;
}

/* ======================================================================
 * Variable store
 * ====================================================================== */

bool var_store_set(var_store_t *store, const char *name, uint64_t value)
{
    if (!store || !name || name[0] == '\0') return false;

    /* Update existing entry if name matches */
    for (size_t i = 0; i < VAR_STORE_SIZE; i++) {
        if (store->entries[i].set &&
            k_strcmp(store->entries[i].name, name) == 0) {
            store->entries[i].value = value;
            return true;
        }
    }

    /* Find an empty slot */
    for (size_t i = 0; i < VAR_STORE_SIZE; i++) {
        if (!store->entries[i].set) {
            safe_strcpy(store->entries[i].name, name, SANDBOX_ARG_MAX_LEN);
            store->entries[i].value = value;
            store->entries[i].set   = true;
            return true;
        }
    }

    return false; /* Store full */
}

bool var_store_get(const var_store_t *store, const char *name, uint64_t *out)
{
    if (!store || !name || !out) return false;

    for (size_t i = 0; i < VAR_STORE_SIZE; i++) {
        if (store->entries[i].set &&
            k_strcmp(store->entries[i].name, name) == 0) {
            *out = store->entries[i].value;
            return true;
        }
    }
    return false;
}

/* ---- Resolve an operand to a uint64 value ------------------------------ */
static bool resolve_operand(const operand_t *op,
                             const var_store_t *store,
                             uint64_t *out)
{
    if (op->kind == OPERAND_LITERAL) {
        *out = op->literal;
        return true;
    }
    return var_store_get(store, op->var_name, out);
}

/* ---- Evaluate a condition ---------------------------------------------- */
static bool eval_cond(const pipeline_cond_t *cond,
                      const var_store_t *store,
                      bool *result)
{
    uint64_t lval, rval;
    if (!resolve_operand(&cond->left,  store, &lval)) return false;
    if (!resolve_operand(&cond->right, store, &rval)) return false;

    switch (cond->op) {
        case CMP_EQ:  *result = (lval == rval); break;
        case CMP_NEQ: *result = (lval != rval); break;
        case CMP_LT:  *result = (lval <  rval); break;
        case CMP_GT:  *result = (lval >  rval); break;
        case CMP_LTE: *result = (lval <= rval); break;
        case CMP_GTE: *result = (lval >= rval); break;
        default: return false;
    }
    return true;
}

/* ======================================================================
 * interpreter_parse — single tool call into ast_node_t
 * ====================================================================== */
bool interpreter_parse(const char *input, ast_node_t *out_node)
{
    if (!input || !out_node) return false;
    k_memset(out_node, 0, sizeof(ast_node_t));
    out_node->opcode = OP_INVALID;

    /* Extract opcode token */
    char opcode_str[SANDBOX_ARG_MAX_LEN];
    size_t i = 0;
    while (input[i] != '\0' && input[i] != ' ' && i < SANDBOX_ARG_MAX_LEN - 1U) {
        opcode_str[i] = input[i]; i++;
    }
    opcode_str[i] = '\0';
    if (i == 0) return false;

    /* Look up opcode */
    bool found = false;
    for (size_t j = 0; j < OPCODE_TABLE_SIZE; j++) {
        if (k_strcmp(opcode_str, opcode_table[j].name) == 0) {
            out_node->opcode = opcode_table[j].opcode;
            found = true;
            break;
        }
    }
    if (!found) return false;

    /* Tokenise arguments */
    const char *cursor = input + i;
    uint32_t argc = 0;

    while (*cursor != '\0' && argc < SANDBOX_MAX_ARGS) {
        if (*cursor == ' ') { cursor++; continue; }

        /* Stop at "->" output binding marker */
        if (cursor[0] == '-' && cursor[1] == '>') break;

        size_t arg_len = 0;
        while (cursor[arg_len] != '\0' &&
               cursor[arg_len] != ' '  &&
               arg_len < SANDBOX_ARG_MAX_LEN - 1U) {
            arg_len++;
        }
        if (arg_len == 0) break;

        safe_strcpy(out_node->args[argc], cursor, SANDBOX_ARG_MAX_LEN);
        cursor += arg_len;
        argc++;
    }

    out_node->argc = argc;
    return true;
}

/* ======================================================================
 * interpreter_parse_pipeline — semicolon-separated steps into pipeline_t
 *
 * Format per step:
 *   <opcode> [arg0] [arg1] [-> varname]
 *
 * Steps are separated by ';'.
 * Output binding "-> varname" is parsed from the end of each step.
 *
 * OP_IF format:
 *   if <left> <op> <right> -> then:<N> [else:<M>]
 *   where N and M are the number of following steps in each branch.
 *   e.g.:  "if 1 == 1 -> then:2 else:1; echo yes; nop; echo no"
 * ====================================================================== */

/* Helper: parse a single step token (up to ';' or end) into pipeline_node_t */
static bool parse_pipeline_step(const char *input, size_t len,
                                 pipeline_node_t *out)
{
    if (!input || len == 0 || !out) return false;

    /* Copy step text into a local NUL-terminated buffer */
    char buf[128];
    if (len >= sizeof(buf)) return false;
    for (size_t i = 0; i < len; i++) buf[i] = input[i];
    buf[len] = '\0';

    k_memset(out, 0, sizeof(pipeline_node_t));
    out->opcode = OP_INVALID;

    /* ---- Check for "-> varname" output binding at end of step ---- */
    /* Scan backwards for "->" */
    int arrow_pos = -1;
    for (int i = (int)len - 1; i >= 1; i--) {
        if (buf[i-1] == '-' && buf[i] == '>') {
            arrow_pos = i - 1;
            break;
        }
    }

    if (arrow_pos >= 0) {
        /* Extract output var name (skip "-> " whitespace) */
        const char *var_start = buf + arrow_pos + 2;
        while (*var_start == ' ') var_start++;

        /* Handle OP_IF branch counts: "then:N" or "else:M" */
        if (k_strncmp(var_start, "then:", 5) == 0) {
            uint64_t then_count;
            if (!parse_uint64(var_start + 5, &then_count)) return false;
            out->then_count = (uint32_t)then_count;

            /* Check for else:M after then:N */
            const char *else_ptr = var_start + 5;
            while (*else_ptr >= '0' && *else_ptr <= '9') else_ptr++;
            while (*else_ptr == ' ') else_ptr++;
            if (k_strncmp(else_ptr, "else:", 5) == 0) {
                uint64_t else_count;
                if (!parse_uint64(else_ptr + 5, &else_count)) return false;
                out->else_count = (uint32_t)else_count;
            }
        } else {
            /* Regular output variable binding */
            safe_strcpy(out->output_var, var_start, SANDBOX_ARG_MAX_LEN);
        }

        /* Truncate buf at the arrow */
        buf[arrow_pos] = '\0';
        len = (size_t)arrow_pos;
    }

    /* ---- Trim trailing spaces ---- */
    while (len > 0 && buf[len - 1] == ' ') { buf[--len] = '\0'; }
    if (len == 0) return false;

    /* ---- Parse opcode ---- */
    char opcode_str[SANDBOX_ARG_MAX_LEN];
    size_t i = 0;
    while (buf[i] != '\0' && buf[i] != ' ' && i < SANDBOX_ARG_MAX_LEN - 1U) {
        opcode_str[i] = buf[i]; i++;
    }
    opcode_str[i] = '\0';
    if (i == 0) return false;

    bool found = false;
    for (size_t j = 0; j < OPCODE_TABLE_SIZE; j++) {
        if (k_strcmp(opcode_str, opcode_table[j].name) == 0) {
            out->opcode = opcode_table[j].opcode;
            found = true;
            break;
        }
    }
    if (!found) return false;

    const char *cursor = buf + i;
    uint32_t argc = 0;

    /* ---- OP_IF: parse condition operands ---- */
    if (out->opcode == OP_IF) {
        /*
         * Format: if <left> <op> <right>
         * left/right are either a number literal or a variable name.
         * op is one of: == != < > <= >=
         */
        while (*cursor == ' ') cursor++;

        /* Parse left operand */
        char left_str[SANDBOX_ARG_MAX_LEN];
        size_t k = 0;
        while (cursor[k] != ' ' && cursor[k] != '\0' && k < SANDBOX_ARG_MAX_LEN - 1U) {
            left_str[k] = cursor[k];
            k++;
        }
        left_str[k] = '\0';
        cursor += k;
        while (*cursor == ' ') cursor++;

        uint64_t lval;
        if (parse_uint64(left_str, &lval)) {
            out->cond.left.kind    = OPERAND_LITERAL;
            out->cond.left.literal = lval;
        } else {
            out->cond.left.kind = OPERAND_VARIABLE;
            safe_strcpy(out->cond.left.var_name, left_str, SANDBOX_ARG_MAX_LEN);
        }

        /* Parse operator */
        char op_str[4] = {0};
        size_t op_len = 0;
        while (cursor[op_len] != ' ' && cursor[op_len] != '\0' && op_len < 3U) {
            op_str[op_len] = cursor[op_len];
            op_len++;
        }
        op_str[op_len] = '\0';
        cursor += op_len;
        while (*cursor == ' ') cursor++;

        if      (k_strcmp(op_str, "==") == 0) out->cond.op = CMP_EQ;
        else if (k_strcmp(op_str, "!=") == 0) out->cond.op = CMP_NEQ;
        else if (k_strcmp(op_str, "<")  == 0) out->cond.op = CMP_LT;
        else if (k_strcmp(op_str, ">")  == 0) out->cond.op = CMP_GT;
        else if (k_strcmp(op_str, "<=") == 0) out->cond.op = CMP_LTE;
        else if (k_strcmp(op_str, ">=") == 0) out->cond.op = CMP_GTE;
        else return false;

        /* Parse right operand */
        char right_str[SANDBOX_ARG_MAX_LEN];
        k = 0;
        while (cursor[k] != ' ' && cursor[k] != '\0' && k < SANDBOX_ARG_MAX_LEN - 1U) {
            right_str[k] = cursor[k];
            k++;
        }
        right_str[k] = '\0';

        uint64_t rval;
        if (parse_uint64(right_str, &rval)) {
            out->cond.right.kind    = OPERAND_LITERAL;
            out->cond.right.literal = rval;
        } else {
            out->cond.right.kind = OPERAND_VARIABLE;
            safe_strcpy(out->cond.right.var_name, right_str, SANDBOX_ARG_MAX_LEN);
        }

        out->argc = 0;
        return true;
    }

    /* ---- Regular opcodes: tokenise arguments ---- */
    while (*cursor != '\0' && argc < SANDBOX_MAX_ARGS) {
        if (*cursor == ' ') { cursor++; continue; }
        size_t arg_len = 0;
        while (cursor[arg_len] != '\0' &&
               cursor[arg_len] != ' '  &&
               arg_len < SANDBOX_ARG_MAX_LEN - 1U) {
            arg_len++;
        }
        if (arg_len == 0) break;
        safe_strcpy(out->args[argc], cursor, SANDBOX_ARG_MAX_LEN);
        cursor += arg_len;
        argc++;
    }
    out->argc = argc;
    return true;
}

bool interpreter_parse_pipeline(const char *input, pipeline_t *out_pipeline)
{
    if (!input || !out_pipeline) return false;
    k_memset(out_pipeline, 0, sizeof(pipeline_t));

    const char *cursor = input;
    uint32_t    step   = 0;

    while (*cursor != '\0' && step < PIPELINE_MAX_STEPS) {
        /* Skip leading spaces */
        while (*cursor == ' ') cursor++;
        if (*cursor == '\0') break;

        /* Find end of this step (next ';' or end of string) */
        const char *start = cursor;
        size_t      len   = 0;
        while (cursor[len] != ';' && cursor[len] != '\0') len++;

        if (len == 0) { cursor++; continue; }

        if (!parse_pipeline_step(start, len, &out_pipeline->steps[step]))
            return false;

        step++;
        cursor = start + len;
        if (*cursor == ';') cursor++;
    }

    out_pipeline->step_count = step;
    return step > 0;
}

/* ======================================================================
 * interpreter_exec — execute one verified ast_node_t
 * ====================================================================== */
sandbox_result_t interpreter_exec(ast_node_t *node, sandbox_ctx_t *ctx)
{
    if (!node || !ctx) return SANDBOX_ERR_UNKNOWN;

    if (ctx->insn_count >= SANDBOX_MAX_INSNS) return SANDBOX_ERR_LIMIT;
    ctx->insn_count++;

    switch (node->opcode) {

        case OP_NOP:
            break;

        case OP_ECHO:
            sys_uart_write(node->args[0], k_strlen(node->args[0]), ctx->caps);
            sys_uart_write("\n", 1, ctx->caps);
            break;

        case OP_READ: {
            uint64_t addr, len;
            parse_uint64(node->args[0], &addr);
            parse_uint64(node->args[1], &len);
            if (sys_mem_read((uintptr_t)addr, ctx->scratch,
                             (size_t)len, ctx->caps) != 0)
                return SANDBOX_ERR_FAULT;
            sys_uart_write("read: ", 6, ctx->caps);
            for (size_t i = 0; i < (size_t)len; i++) {
                uint8_t b  = ctx->scratch[i];
                char    hi = (b >> 4) < 10u ? (char)('0'+(b>>4))
                                             : (char)('A'+(b>>4)-10);
                char    lo = (b & 0xF) < 10u ? (char)('0'+(b&0xF))
                                              : (char)('A'+(b&0xF)-10);
                sys_uart_write(&hi, 1, ctx->caps);
                sys_uart_write(&lo, 1, ctx->caps);
                sys_uart_write(" ",  1, ctx->caps);
            }
            sys_uart_write("\n", 1, ctx->caps);
            ctx->scratch_used = (size_t)len;
            break;
        }

        case OP_WRITE: {
            uint64_t offset, value;
            parse_uint64(node->args[0], &offset);
            parse_uint64(node->args[1], &value);
            ctx->scratch[offset] = (uint8_t)value;
            if (offset >= ctx->scratch_used) ctx->scratch_used = offset + 1U;
            break;
        }

        case OP_INFO: {
            uintptr_t bs, be, ss, se;
            if (sys_mem_info(&bs, &be, &ss, &se, ctx->caps) != 0)
                return SANDBOX_ERR_DENIED;
            sys_uart_write("BSS   start : ", 14, ctx->caps); sys_uart_hex64(bs, ctx->caps);
            sys_uart_write("\nBSS   end   : ", 15, ctx->caps); sys_uart_hex64(be, ctx->caps);
            sys_uart_write("\nStack start : ", 15, ctx->caps); sys_uart_hex64(ss, ctx->caps);
            sys_uart_write("\nStack end   : ", 15, ctx->caps); sys_uart_hex64(se, ctx->caps);
            sys_uart_write("\n", 1, ctx->caps);
            break;
        }

        case OP_EL: {
            uint64_t el;
            __asm__ volatile ("mrs %0, CurrentEL" : "=r" (el));
            uint32_t el_val = (uint32_t)((el >> 2U) & 3U);
            sys_uart_write("EL: ", 4, ctx->caps);
            char c = (char)('0' + el_val);
            sys_uart_write(&c, 1, ctx->caps);
            sys_uart_write("\n", 1, ctx->caps);
            break;
        }

        case OP_CAPS:
            sys_uart_write("caps: ", 6, ctx->caps);
            sys_uart_hex64((uint64_t)ctx->caps, ctx->caps);
            sys_uart_write("\n", 1, ctx->caps);
            break;

        default:
            return SANDBOX_ERR_UNKNOWN;
    }

    return SANDBOX_OK;
}

/* ======================================================================
 * interpreter_exec_pipeline — execute a verified pipeline_t
 *
 * Steps execute in order. OP_IF evaluates its condition against the
 * variable store and skips then_count or else_count steps accordingly.
 * Output bindings store the insn_count at time of execution as a
 * simple numeric result that later steps can reference.
 * ====================================================================== */
sandbox_result_t interpreter_exec_pipeline(pipeline_t *pipeline,
                                            sandbox_ctx_t *ctx)
{
    if (!pipeline || !ctx) return SANDBOX_ERR_UNKNOWN;

    uint32_t i = 0;

    while (i < pipeline->step_count) {
        if (ctx->insn_count >= SANDBOX_MAX_INSNS) return SANDBOX_ERR_LIMIT;

        pipeline_node_t *step = &pipeline->steps[i];

        /* ---- OP_IF: evaluate condition, skip branches ---------------- */
        if (step->opcode == OP_IF) {
            ctx->insn_count++;
            bool cond_result = false;
            if (!eval_cond(&step->cond, &ctx->vars, &cond_result))
                return SANDBOX_ERR_COND;

            if (cond_result) {
                /* Execute then-branch steps — they follow immediately */
                i++;
                /* Steps i .. i+then_count-1 will execute normally in
                 * subsequent loop iterations; skip else-branch after */
                /* Mark else steps to skip by jumping past them after then */
                /* We handle this by letting the loop run then-branch
                 * naturally, then inserting a skip after */
                uint32_t then_end  = i + step->then_count;
                uint32_t else_skip = step->else_count;

                /* Execute then steps */
                while (i < then_end && i < pipeline->step_count) {
                    ast_node_t tmp;
                    k_memset(&tmp, 0, sizeof(ast_node_t));
                    tmp.opcode = pipeline->steps[i].opcode;
                    tmp.argc   = pipeline->steps[i].argc;
                    for (uint32_t a = 0; a < pipeline->steps[i].argc; a++)
                        safe_strcpy(tmp.args[a], pipeline->steps[i].args[a],
                                    SANDBOX_ARG_MAX_LEN);

                    sandbox_result_t r = interpreter_exec(&tmp, ctx);
                    if (r != SANDBOX_OK) return r;

                    /* Output binding */
                    if (pipeline->steps[i].output_var[0] != '\0') {
                        if (!var_store_set(&ctx->vars,
                                           pipeline->steps[i].output_var,
                                           (uint64_t)ctx->insn_count))
                            return SANDBOX_ERR_VAR;
                    }
                    i++;
                }
                /* Skip else-branch */
                i += else_skip;
            } else {
                /* Skip then-branch, execute else-branch if present */
                i += step->then_count + 1;

                uint32_t else_end = i + step->else_count;
                while (i < else_end && i < pipeline->step_count) {
                    ast_node_t tmp;
                    k_memset(&tmp, 0, sizeof(ast_node_t));
                    tmp.opcode = pipeline->steps[i].opcode;
                    tmp.argc   = pipeline->steps[i].argc;
                    for (uint32_t a = 0; a < pipeline->steps[i].argc; a++)
                        safe_strcpy(tmp.args[a], pipeline->steps[i].args[a],
                                    SANDBOX_ARG_MAX_LEN);

                    sandbox_result_t r = interpreter_exec(&tmp, ctx);
                    if (r != SANDBOX_OK) return r;

                    if (pipeline->steps[i].output_var[0] != '\0') {
                        if (!var_store_set(&ctx->vars,
                                           pipeline->steps[i].output_var,
                                           (uint64_t)ctx->insn_count))
                            return SANDBOX_ERR_VAR;
                    }
                    i++;
                }
            }
            continue;
        }

        /* ---- Regular step ------------------------------------------- */
        ast_node_t tmp;
        k_memset(&tmp, 0, sizeof(ast_node_t));
        tmp.opcode = step->opcode;
        tmp.argc   = step->argc;
        for (uint32_t a = 0; a < step->argc; a++)
            safe_strcpy(tmp.args[a], step->args[a], SANDBOX_ARG_MAX_LEN);

        sandbox_result_t r = interpreter_exec(&tmp, ctx);
        if (r != SANDBOX_OK) return r;

        /* Output binding */
        if (step->output_var[0] != '\0') {
            if (!var_store_set(&ctx->vars, step->output_var,
                               (uint64_t)ctx->insn_count))
                return SANDBOX_ERR_VAR;
        }

        i++;
    }

    return SANDBOX_OK;
}