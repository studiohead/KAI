/*
 * src/sandbox/verifier.c — KAI OS sandbox pre-execution verifier
 *
 * Validates an AST node before the interpreter is allowed to execute it.
 * The verifier is the security boundary — if it passes a node, the
 * interpreter trusts it completely. If it rejects, nothing executes.
 *
 * Checks performed:
 *   1. Opcode is within the legal range (< OP_INVALID)
 *   2. Argument count matches what the opcode expects
 *   3. For OP_READ: address is within a whitelisted memory region
 *   4. For OP_WRITE: address is within the scratch buffer bounds
 *   5. For OP_ECHO: argument length is within SANDBOX_ARG_MAX_LEN
 *   6. Required capabilities are present for the requested opcode
 */

#include <kernel/sandbox.h>
#include <kernel/memory.h>
#include <kernel/string.h>
#include <stdbool.h>
#include <stdint.h>

/* ---- Opcode capability requirements ------------------------------------- */
/*
 * Maps each opcode to the capability bits it requires.
 * The verifier rejects any opcode whose required caps are not in the
 * session mask before the interpreter ever sees the node.
 */
static const uint32_t opcode_required_caps[OP_INVALID] = {
    [OP_NOP]   = CAP_NONE,
    [OP_READ]  = CAP_READ_MEM,
    [OP_WRITE] = CAP_WRITE_MEM,
    [OP_INFO]  = CAP_READ_MEM,
    [OP_ECHO]  = CAP_MMIO,
    [OP_EL]    = CAP_MMIO,
    [OP_CAPS]  = CAP_MMIO,
};

/* ---- Opcode expected argument counts ------------------------------------ */
static const uint32_t opcode_expected_argc[OP_INVALID] = {
    [OP_NOP]   = 0,
    [OP_READ]  = 2,   /* address, length */
    [OP_WRITE] = 2,   /* offset into scratch, byte value */
    [OP_INFO]  = 0,
    [OP_ECHO]  = 1,   /* string to print */
    [OP_EL]    = 0,
    [OP_CAPS]  = 0,
};

/* ---- Helper: parse hex or decimal string to uint64 ---------------------- */
/*
 * Accepts "0x..." for hex and plain digits for decimal.
 * Returns false if the string contains invalid characters.
 */
static bool parse_uint64(const char *s, uint64_t *out)
{
    if (!s || !out || s[0] == '\0')
        return false;

    uint64_t result = 0;

    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        /* Hexadecimal */
        s += 2;
        if (s[0] == '\0')
            return false;
        while (*s) {
            uint8_t nibble;
            if (*s >= '0' && *s <= '9')      nibble = (uint8_t)(*s - '0');
            else if (*s >= 'a' && *s <= 'f') nibble = (uint8_t)(*s - 'a' + 10);
            else if (*s >= 'A' && *s <= 'F') nibble = (uint8_t)(*s - 'A' + 10);
            else return false;
            result = (result << 4) | nibble;
            s++;
        }
    } else {
        /* Decimal */
        while (*s) {
            if (*s < '0' || *s > '9')
                return false;
            result = result * 10U + (uint64_t)(*s - '0');
            s++;
        }
    }

    *out = result;
    return true;
}

/* ======================================================================
 * verifier_check
 *
 * Returns true if the AST node is safe to execute, false otherwise.
 * ====================================================================== */
bool verifier_check(const ast_node_t *node, uint32_t caps)
{
    if (!node)
        return false;

    /* ---- 1. Opcode range check ---------------------------------------- */
    if (node->opcode >= OP_INVALID)
        return false;

    /* ---- 2. Capability check ------------------------------------------ */
    uint32_t required = opcode_required_caps[node->opcode];
    if ((caps & required) != required)
        return false;

    /* ---- 3. Argument count check -------------------------------------- */
    if (node->argc != opcode_expected_argc[node->opcode])
        return false;

    /* ---- 4. Opcode-specific argument validation ----------------------- */
    switch (node->opcode) {

        case OP_READ: {
            /*
             * arg[0] = address (hex or decimal)
             * arg[1] = length in bytes
             * Entire range [addr, addr+len) must be whitelisted.
             */
            uint64_t addr, len;
            if (!parse_uint64(node->args[0], &addr)) return false;
            if (!parse_uint64(node->args[1], &len))  return false;
            if (len == 0 || len > SANDBOX_SCRATCH_SIZE) return false;

            /* Check overflow */
            if (addr + len < addr) return false;

            /* Verify every byte of the range is in a whitelisted region */
            for (uint64_t i = 0; i < len; i++) {
                if (!is_allowed_addr((uintptr_t)(addr + i)))
                    return false;
            }
            break;
        }

        case OP_WRITE: {
            /*
             * arg[0] = offset into scratch buffer
             * arg[1] = byte value (0–255)
             * Offset must be within SANDBOX_SCRATCH_SIZE.
             */
            uint64_t offset, value;
            if (!parse_uint64(node->args[0], &offset)) return false;
            if (!parse_uint64(node->args[1], &value))  return false;
            if (offset >= SANDBOX_SCRATCH_SIZE) return false;
            if (value  >  0xFFU)               return false;
            break;
        }

        case OP_ECHO: {
            /*
             * arg[0] = string to echo
             * Must be non-empty and within length limit.
             */
            size_t len = k_strlen(node->args[0]);
            if (len == 0 || len >= SANDBOX_ARG_MAX_LEN)
                return false;
            break;
        }

        case OP_NOP:
        case OP_INFO:
        case OP_EL:
        case OP_CAPS:
            /* No operand validation needed for zero-argument opcodes */
            break;

        default:
            return false;
    }

    return true;
}