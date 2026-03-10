/*
 * src/sandbox/verifier.c — KAI OS sandbox pre-execution verifier
 *
 * Two entry points:
 *   verifier_check          — validates a single ast_node_t
 *   verifier_check_pipeline — validates every step in a pipeline_t
 *
 * The verifier is the security boundary. Nothing executes unless it
 * passes here first. The interpreter trusts verified input completely.
 *
 * Checks performed per node:
 *   1. Opcode is within the legal range (< OP_INVALID)
 *   2. Required capabilities are present
 *   3. Argument count matches what the opcode expects
 *   4. Opcode-specific argument validation:
 *        OP_READ  — address range fully within a whitelisted region
 *        OP_WRITE — offset within SANDBOX_SCRATCH_SIZE, value 0-255
 *        OP_ECHO  — argument non-empty and within length limit
 *        OP_IF    — then_count > 0, else_count fits within pipeline bounds
 *
 * Pipeline-level checks:
 *   - step_count <= PIPELINE_MAX_STEPS
 *   - Every step passes verifier_check
 *   - OP_IF branch counts don't reach past the end of the pipeline
 *   - Total step count doesn't exceed SANDBOX_MAX_INSNS
 */

#include <kernel/sandbox.h>
#include <kernel/memory.h>
#include <kernel/string.h>
#include <stdbool.h>
#include <stdint.h>

/* ---- Opcode capability requirements ------------------------------------- */
static const uint32_t opcode_required_caps[OP_INVALID] = {
    [OP_NOP]         = CAP_NONE,
    [OP_READ]        = CAP_READ_MEM,
    [OP_WRITE]       = CAP_WRITE_MEM,
    [OP_INFO]        = CAP_READ_MEM,
    [OP_ECHO]        = CAP_MMIO,
    [OP_EL]          = CAP_MMIO,
    [OP_CAPS]        = CAP_MMIO,
    [OP_IF]          = CAP_NONE,   /* Caps checked on branch steps individually */
    [OP_SLEEP]       = CAP_NONE,   /* Timing only — no privileged access        */
    [OP_INTROSPECT]  = CAP_MMIO,   /* Prints MMIO map to UART                   */
    [OP_WAIT_EVENT]  = CAP_NONE,   /* Yield stub — no privileged access         */
    [OP_RESPOND]    = CAP_MMIO,   /* Emits JSON packet over UART               */
};

/* ---- Opcode expected argument counts ------------------------------------ */
static const uint32_t opcode_expected_argc[OP_INVALID] = {
    [OP_NOP]        = 0,
    [OP_READ]       = 2,   /* address, length                         */
    [OP_WRITE]      = 2,   /* scratch offset, byte value              */
    [OP_INFO]       = 0,
    [OP_ECHO]       = 1,   /* string to print                         */
    [OP_EL]         = 0,
    [OP_CAPS]       = 0,
    [OP_IF]         = 0,   /* Condition is in cond field, not args    */
    [OP_SLEEP]      = 1,   /* milliseconds (0–10000)                  */
    [OP_INTROSPECT] = 0,   /* No args — prints full MMIO map          */
    [OP_WAIT_EVENT] = 0,   /* No args — yields until next event       */
    [OP_RESPOND]   = 0,   /* Optional goal label in args[0]          */
};

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

/* ---- Helper: validate a single operand ---------------------------------- */
/*
 * A LITERAL operand must parse as a valid uint64.
 * A VARIABLE operand must have a non-empty name — we don't resolve at
 * verify time (the variable may be bound by an earlier pipeline step),
 * but we confirm the name is syntactically valid.
 */
static bool verify_operand(const operand_t *op)
{
    if (op->kind == OPERAND_LITERAL) {
        /* Literal is already stored as uint64 — always valid */
        return true;
    }
    /* Variable: name must be non-empty */
    return op->var_name[0] != '\0';
}

/* ---- Helper: validate a condition (BinaryExpression) ------------------- */
static bool verify_cond(const pipeline_cond_t *cond)
{
    if (!verify_operand(&cond->left))  return false;
    if (!verify_operand(&cond->right)) return false;
    if ((uint32_t)cond->op > (uint32_t)CMP_GTE) return false;
    return true;
}

/* ======================================================================
 * verifier_check — validate a single ast_node_t
 * ====================================================================== */
bool verifier_check(const ast_node_t *node, uint32_t caps)
{
    if (!node) return false;

    /* 1. Opcode range */
    if (node->opcode >= OP_INVALID) return false;

    /* 2. Capability */
    uint32_t required = opcode_required_caps[node->opcode];
    if ((caps & required) != required) return false;

    /* 3. Argument count — OP_RESPOND accepts 0 or 1 (optional goal label) */
    if (node->opcode == OP_RESPOND) {
        if (node->argc > 1U) return false;
    } else {
        if (node->argc != opcode_expected_argc[node->opcode]) return false;
    }

    /* 4. Opcode-specific */
    switch (node->opcode) {

        case OP_READ: {
            uint64_t addr, len;
            if (!parse_uint64(node->args[0], &addr)) return false;
            if (!parse_uint64(node->args[1], &len))  return false;
            if (len == 0 || len > SANDBOX_SCRATCH_SIZE) return false;
            if (addr + len < addr) return false; /* overflow */
            for (uint64_t i = 0; i < len; i++) {
                if (!is_allowed_addr((uintptr_t)(addr + i))) return false;
            }
            break;
        }

        case OP_WRITE: {
            uint64_t offset, value;
            if (!parse_uint64(node->args[0], &offset)) return false;
            if (!parse_uint64(node->args[1], &value))  return false;
            if (offset >= SANDBOX_SCRATCH_SIZE) return false;
            if (value  >  0xFFU)               return false;
            break;
        }

        case OP_ECHO: {
            size_t len = k_strlen(node->args[0]);
            if (len == 0 || len >= SANDBOX_ARG_MAX_LEN) return false;
            break;
        }

        case OP_NOP:
        case OP_INFO:
        case OP_EL:
        case OP_CAPS:
        case OP_INTROSPECT:
        case OP_WAIT_EVENT:
            break;

        case OP_SLEEP: {
            /* Clamp to a safe maximum — 10 seconds prevents runaway waits */
            uint64_t ms;
            if (!parse_uint64(node->args[0], &ms)) return false;
            if (ms > 10000U) return false;  /* 10 000 ms = 10 s maximum   */
            break;
        }

        case OP_IF:
            /* OP_IF is valid in ast_node_t only structurally —
             * branch body validation happens at pipeline level */
            break;

        default:
            return false;
    }

    return true;
}

/* ======================================================================
 * verifier_check_pipeline — validate every step in a pipeline_t
 * ====================================================================== */
bool verifier_check_pipeline(const pipeline_t *pipeline, uint32_t caps)
{
    if (!pipeline) return false;
    if (pipeline->step_count == 0 ||
        pipeline->step_count > PIPELINE_MAX_STEPS) return false;

    /* Total instruction budget check */
    if (pipeline->step_count > SANDBOX_MAX_INSNS) return false;

    for (uint32_t i = 0; i < pipeline->step_count; i++) {
        const pipeline_node_t *step = &pipeline->steps[i];

        /* Opcode range */
        if (step->opcode >= OP_INVALID) return false;

        /* Capability */
        uint32_t required = opcode_required_caps[step->opcode];
        if ((caps & required) != required) return false;

        /* Argument count */
        if (step->argc != opcode_expected_argc[step->opcode]) return false;

        /* OP_IF: validate condition and branch counts */
        if (step->opcode == OP_IF) {
            if (!verify_cond(&step->cond))    return false;
            if (step->then_count == 0)        return false;

            /* then-branch must not reach past end of pipeline */
            if (i + 1 + step->then_count > pipeline->step_count)
                return false;

            /* else-branch (if present) must also fit */
            if (step->else_count > 0) {
                if (i + 1 + step->then_count + step->else_count >
                    pipeline->step_count)
                    return false;
            }
            continue;   /* Branch body steps are validated in their own iterations */
        }

        /* OP_READ: full address range whitelist check */
        if (step->opcode == OP_READ) {
            uint64_t addr, len;
            if (!parse_uint64(step->args[0], &addr)) return false;
            if (!parse_uint64(step->args[1], &len))  return false;
            if (len == 0 || len > SANDBOX_SCRATCH_SIZE) return false;
            if (addr + len < addr) return false;
            for (uint64_t j = 0; j < len; j++) {
                if (!is_allowed_addr((uintptr_t)(addr + j))) return false;
            }
            continue;
        }

        /* OP_WRITE: offset and value bounds */
        if (step->opcode == OP_WRITE) {
            uint64_t offset, value;
            if (!parse_uint64(step->args[0], &offset)) return false;
            if (!parse_uint64(step->args[1], &value))  return false;
            if (offset >= SANDBOX_SCRATCH_SIZE) return false;
            if (value  >  0xFFU)               return false;
            continue;
        }

        /* OP_ECHO: non-empty, within length limit */
        if (step->opcode == OP_ECHO) {
            size_t len = k_strlen(step->args[0]);
            if (len == 0 || len >= SANDBOX_ARG_MAX_LEN) return false;
        }

        /* OP_SLEEP: clamp to safe maximum */
        if (step->opcode == OP_SLEEP) {
            uint64_t ms;
            if (!parse_uint64(step->args[0], &ms)) return false;
            if (ms > 10000U) return false;
        }

        /* OP_INTROSPECT, OP_WAIT_EVENT, OP_NOP, OP_INFO, OP_EL, OP_CAPS:
         * no opcode-specific argument validation needed */
    }

    return true;
}