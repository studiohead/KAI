/*
 * src/sandbox/interpreter.c — KAI OS sandbox interpreter
 *
 * Responsible for two things:
 *   1. interpreter_parse  — tokenise a raw tool_call string into an ast_node_t
 *   2. interpreter_exec   — execute a verified ast_node_t within a sandbox_ctx_t
 *
 * The interpreter trusts that verifier_check has already been called.
 * It does not re-verify — it executes. All safety guarantees come from
 * the verifier running first.
 *
 * Input format:
 *   <opcode_name> [arg0] [arg1] ...
 *   Tokens are separated by single spaces. Extra whitespace is not supported.
 *
 * Examples:
 *   "nop"
 *   "echo hello"
 *   "read 0x40001000 16"
 *   "write 0 0xFF"
 *   "info"
 *   "el"
 *   "caps"
 */

#include <kernel/sandbox.h>
#include <kernel/syscall.h>
#include <kernel/memory.h>
#include <kernel/string.h>
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* ---- Opcode name table -------------------------------------------------- */
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
};

#define OPCODE_TABLE_SIZE (sizeof(opcode_table) / sizeof(opcode_table[0]))

/* ---- Helper: copy at most n-1 bytes, always NUL-terminate -------------- */
static void safe_strcpy(char *dst, const char *src, size_t max)
{
    size_t i = 0;
    while (i < max - 1U && src[i] != '\0') {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
}

/* ---- Helper: parse hex or decimal string to uint64 --------------------- */
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
 * interpreter_parse
 *
 * Tokenises 'input' into an ast_node_t.
 * Returns true on success, false if the input is malformed or the
 * opcode is unrecognised.
 * ====================================================================== */
bool interpreter_parse(const char *input, ast_node_t *out_node)
{
    if (!input || !out_node)
        return false;

    /* Zero the output node */
    k_memset(out_node, 0, sizeof(ast_node_t));
    out_node->opcode = OP_INVALID;

    /* ---- Extract the opcode token (first word) ------------------------ */
    char opcode_str[SANDBOX_ARG_MAX_LEN];
    size_t i = 0;

    while (input[i] != '\0' && input[i] != ' ' && i < SANDBOX_ARG_MAX_LEN - 1U) {
        opcode_str[i] = input[i];
        i++;
    }
    opcode_str[i] = '\0';

    if (i == 0)
        return false;   /* Empty input */

    /* ---- Look up opcode name ------------------------------------------ */
    bool found = false;
    for (size_t j = 0; j < OPCODE_TABLE_SIZE; j++) {
        if (k_strcmp(opcode_str, opcode_table[j].name) == 0) {
            out_node->opcode = opcode_table[j].opcode;
            found = true;
            break;
        }
    }

    if (!found)
        return false;

    /* ---- Tokenise remaining arguments --------------------------------- */
    const char *cursor = input + i;
    uint32_t argc = 0;

    while (*cursor != '\0' && argc < SANDBOX_MAX_ARGS) {
        /* Skip leading space */
        if (*cursor == ' ') {
            cursor++;
            continue;
        }

        /* Copy token into args[argc] */
        size_t arg_len = 0;
        while (cursor[arg_len] != '\0' &&
               cursor[arg_len] != ' '  &&
               arg_len < SANDBOX_ARG_MAX_LEN - 1U) {
            arg_len++;
        }

        if (arg_len == 0)
            break;

        safe_strcpy(out_node->args[argc], cursor, SANDBOX_ARG_MAX_LEN);
        cursor  += arg_len;
        argc++;
    }

    out_node->argc = argc;
    return true;
}

/* ======================================================================
 * interpreter_exec
 *
 * Executes a pre-verified AST node within the sandbox context.
 * Increments ctx->insn_count and enforces SANDBOX_MAX_INSNS.
 * ====================================================================== */
sandbox_result_t interpreter_exec(ast_node_t *node, sandbox_ctx_t *ctx)
{
    if (!node || !ctx)
        return SANDBOX_ERR_UNKNOWN;

    /* Hard instruction count limit */
    if (ctx->insn_count >= SANDBOX_MAX_INSNS)
        return SANDBOX_ERR_LIMIT;

    ctx->insn_count++;

    switch (node->opcode) {

        /* ---------------------------------------------------------------- */
        case OP_NOP:
            /* Intentional no-op */
            break;

        /* ---------------------------------------------------------------- */
        case OP_ECHO: {
            sys_uart_write(node->args[0], k_strlen(node->args[0]), ctx->caps);
            sys_uart_write("\n", 1, ctx->caps);
            break;
        }

        /* ---------------------------------------------------------------- */
        case OP_READ: {
            uint64_t addr, len;
            parse_uint64(node->args[0], &addr);
            parse_uint64(node->args[1], &len);

            /* Copy from whitelisted address into scratch buffer */
            int result = sys_mem_read((uintptr_t)addr,
                                      ctx->scratch,
                                      (size_t)len,
                                      ctx->caps);
            if (result != 0)
                return SANDBOX_ERR_FAULT;

            /* Print scratch contents as hex bytes */
            sys_uart_write("read: ", 6, ctx->caps);
            for (size_t i = 0; i < (size_t)len; i++) {
                uint8_t byte = ctx->scratch[i];
                char hi = (byte >> 4) < 10u ? (char)('0' + (byte >> 4))
                                             : (char)('A' + (byte >> 4) - 10);
                char lo = (byte & 0xF) < 10u ? (char)('0' + (byte & 0xF))
                                              : (char)('A' + (byte & 0xF) - 10);
                sys_uart_write(&hi, 1, ctx->caps);
                sys_uart_write(&lo, 1, ctx->caps);
                sys_uart_write(" ",  1, ctx->caps);
            }
            sys_uart_write("\n", 1, ctx->caps);
            ctx->scratch_used = (size_t)len;
            break;
        }

        /* ---------------------------------------------------------------- */
        case OP_WRITE: {
            uint64_t offset, value;
            parse_uint64(node->args[0], &offset);
            parse_uint64(node->args[1], &value);

            /* Verifier already confirmed offset < SANDBOX_SCRATCH_SIZE */
            ctx->scratch[offset] = (uint8_t)value;
            if (offset >= ctx->scratch_used)
                ctx->scratch_used = offset + 1U;
            break;
        }

        /* ---------------------------------------------------------------- */
        case OP_INFO: {
            uintptr_t bss_start, bss_end, stack_start, stack_end;
            int r = sys_mem_info(&bss_start, &bss_end,
                                 &stack_start, &stack_end,
                                 ctx->caps);
            if (r != 0)
                return SANDBOX_ERR_DENIED;

            sys_uart_write("BSS   start : ", 14, ctx->caps);
            sys_uart_hex64(bss_start, ctx->caps);
            sys_uart_write("\nBSS   end   : ", 15, ctx->caps);
            sys_uart_hex64(bss_end, ctx->caps);
            sys_uart_write("\nStack start : ", 15, ctx->caps);
            sys_uart_hex64(stack_start, ctx->caps);
            sys_uart_write("\nStack end   : ", 15, ctx->caps);
            sys_uart_hex64(stack_end, ctx->caps);
            sys_uart_write("\n", 1, ctx->caps);
            break;
        }

        /* ---------------------------------------------------------------- */
        case OP_EL: {
            uint64_t el;
            __asm__ volatile ("mrs %0, CurrentEL" : "=r" (el));
            uint32_t el_val = (uint32_t)((el >> 2U) & 3U);
            sys_uart_write("Exception level: EL", 19, ctx->caps);
            char el_char = (char)('0' + el_val);
            sys_uart_write(&el_char, 1, ctx->caps);
            sys_uart_write("\n", 1, ctx->caps);
            break;
        }

        /* ---------------------------------------------------------------- */
        case OP_CAPS: {
            sys_uart_write("Session caps: ", 14, ctx->caps);
            sys_uart_hex64((uint64_t)ctx->caps, ctx->caps);
            sys_uart_write("\n", 1, ctx->caps);
            break;
        }

        /* ---------------------------------------------------------------- */
        default:
            return SANDBOX_ERR_UNKNOWN;
    }

    return SANDBOX_OK;
}