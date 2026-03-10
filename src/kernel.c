/*
 * src/kernel.c — Kernel main entry point and command shell with AI session support
 *
 * kernel_main() is called by boot.S after the stack and BSS are ready.
 * Hardware drivers (UART, etc.) are fully separated into their own
 * translation units; only their public headers are included here.
 *
 * This version adds AI session awareness:
 * - Each session has a capabilities mask controlling which syscalls are allowed
 * - Command handlers receive a session pointer
 * - Syscalls now use session->caps instead of fixed SYS_CALL_CAPS
 */

/* ---- Standard includes & helper macros -------------------------------- */
#include <kernel/sandbox.h>
#include <kernel/intent.h>
#include <kernel/memory.h>
#include <kernel/kai_node.h>
#include <kernel/kai_interner.h>
#include <kernel/kai_dag.h>
#include <kernel/kai_scheduler.h>
#include <kernel/mmu.h>
#include <kernel/irq.h>
#include <kernel/string.h>
#include <kernel/syscall.h>
#include <kernel/uart.h>
#include <kernel/aiql.h>
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#define COL_RESET   "\033[0m"
#define COL_BOLD    "\033[1m"
#define COL_GREEN   "\033[32m"
#define COL_CYAN    "\033[36m"
#define COL_YELLOW  "\033[33m"
#define COL_RED     "\033[31m"
#define COL_BLUE    "\033[34m"


/* External assembly function from src/arch/aarch64/mmu.S */
extern void vbar_install(void);

/* Helper to get string length for sys_uart_write without hardcoding */
#define KSTRLEN(str) ((size_t)k_strlen(str))

/* ---- Configuration --------------------------------------------------- */
#define CMD_BUF_SIZE    128U    /* Max bytes per command line (incl. NUL) */
#define PROMPT      COL_BOLD COL_GREEN "kai" COL_RESET COL_CYAN "# " COL_RESET

/* Command history */
#define HISTORY_SIZE 8
static char history[HISTORY_SIZE][CMD_BUF_SIZE];
static uint8_t hist_idx = 0;
static uint8_t hist_count = 0;
static int hist_view = -1;   /* -1 = currently typing */

/* ---- AI Session Struct ----------------------------------------------- */
typedef struct {
    uint32_t caps;                 /* Allowed syscalls / capabilities */
} ai_session_t;

/* ---- Sandbox context (global so irq_dispatch can access it) ---------- */
/* Using the global context allows hardware interrupts to share state 
 * with the user shell, enabling 'reflexes' that the user can monitor. */
static sandbox_ctx_t sb_ctx;

/* ---- KAI IR globals -------------------------------------------------- */
/* The interner is global so nodes are deduplicated across successive 'dag'
 * commands in the same session. The DAG is rebuilt per invocation. */
static kai_interner_t kai_intern;

/* ---- Command typedef must come first --------------------------------- */
typedef struct {
    const char *name;
    const char *help;
    void (*handler)(const char *args, ai_session_t *session);
} command_t;

/* ---- Private helpers ------------------------------------------------- */

/* Read the current Exception Level (EL1–EL3) */
static uint32_t current_el(void)
{
    uint64_t el;
    __asm__ volatile ("mrs %0, CurrentEL" : "=r" (el));
    return (uint32_t)((el >> 2U) & 3U);
}

/* Returns true if the character is printable (safe to echo/store) */
static bool is_printable(char c)
{
    return (unsigned char)c >= 0x20U && (unsigned char)c < 0x7FU;
}

/* ---- Forward declarations of handlers -------------------------------- */
static void cmd_help(const char *args, ai_session_t *session);
static void cmd_clear(const char *args, ai_session_t *session);
static void cmd_el(const char *args, ai_session_t *session);
static void cmd_hex(const char *args, ai_session_t *session);
static void cmd_mem(const char *args, ai_session_t *session);
static void cmd_echo(const char *args, ai_session_t *session);
static void cmd_sandbox(const char *args, ai_session_t *session);
static void cmd_pipeline(const char *args, ai_session_t *session);
static void cmd_dag(const char *args, ai_session_t *session);
static void cmd_irq(const char *args, ai_session_t *session);
static void cmd_irq_bind(const char *args, ai_session_t *session);
static void cmd_aiql(const char *args, ai_session_t *session);
static void cmd_aiql_bind(const char *args, ai_session_t *session);
static void cmd_timer_bind(const char *args, ai_session_t *session);
static void cmd_irq_list(const char *args, ai_session_t *session);

/* ---- Command table --------------------------------------------------- */
static const command_t commands[] = {
    { "help",     "Show this help text",                     cmd_help     },
    { "clear",    "Clear the terminal screen",               cmd_clear    },
    { "el",       "Print current exception level",           cmd_el       },
    { "hex",      "Print an example hex value",              cmd_hex      },
    { "mem",      "Print BSS and stack addresses",           cmd_mem      },
    { "echo",     "Echo text back to the terminal",          cmd_echo     },
    { "sandbox",  "Run a sandboxed tool call",               cmd_sandbox  },
    { "pipeline", "Run a multi-step AIQL pipeline",          cmd_pipeline },
    { "dag",      "Build DAG from pipeline and show schedule", cmd_dag     },
    { "irq_init", "Enable CPU IRQ delivery (start reflexes)", cmd_irq      },
    { "irq_bind", "Bind IRQ <num> to <pipeline>",            cmd_irq_bind },
    { "aiql",       "Execute an AIQL JSON program directly",       cmd_aiql      },
    { "aiql_bind",  "Bind AIQL JSON to IRQ: aiql_bind <irq> <json>",  cmd_aiql_bind },
    { "timer_bind", "Arm timer reflex: timer_bind <ms> <json>",        cmd_timer_bind},
    { "irq_list",   "List all active IRQ->AIQL bindings",              cmd_irq_list  },
};

#define NUM_COMMANDS  (sizeof(commands) / sizeof(commands[0]))

/* ---- Command Handlers ------------------------------------------------ */

/* Clear terminal screen */
static void cmd_clear(const char *args, ai_session_t *session)
{
    (void)args;
    sys_uart_write("\033[2J\033[H", KSTRLEN("\033[2J\033[H"), session->caps);
}

/* Print current exception level */
static void cmd_el(const char *args, ai_session_t *session)
{
    (void)args;
    sys_uart_write("Current privilege level: EL", 27, session->caps);

    char el_char = '0' + (char)current_el();
    sys_uart_write(&el_char, 1, session->caps);
    sys_uart_write("\r\n", 2, session->caps);
}

/* Print example 64-bit hex value */
static void cmd_hex(const char *args, ai_session_t *session)
{
    (void)args;
    sys_uart_write("Example 64-bit hex value: ", KSTRLEN("Example 64-bit hex value: "), session->caps);
    sys_uart_hex64(0xDEADBEEFCAFEBABEULL, session->caps);
    sys_uart_write("\r\n", 2, session->caps);
}

/* Print memory info via syscalls */
static void cmd_mem(const char *args, ai_session_t *session)
{
    (void)args;
    uintptr_t bss_start, bss_end, stack_start, stack_end;

    if (sys_mem_info(&bss_start, &bss_end, &stack_start, &stack_end, session->caps) == 0) {
        sys_uart_write("BSS start   : ", KSTRLEN("BSS start   : "), session->caps);
        sys_uart_hex64(bss_start, session->caps);
        sys_uart_write("\r\nBSS end     : ", KSTRLEN("\r\nBSS end     : "), session->caps);
        sys_uart_hex64(bss_end, session->caps);
        sys_uart_write("\r\nStack start : ", KSTRLEN("\r\nStack start : "), session->caps);
        sys_uart_hex64(stack_start, session->caps);
        sys_uart_write("\r\nStack end   : ", KSTRLEN("\r\nStack end   : "), session->caps);
        sys_uart_hex64(stack_end, session->caps);
        sys_uart_write("\r\n", 2, session->caps);
    } else {
        sys_uart_write("Memory info not allowed\r\n", KSTRLEN("Memory info not allowed\r\n"), session->caps);
    }
}

/* Echo command */
static void cmd_echo(const char *args, ai_session_t *session)
{
    if (!args) return;
    size_t len = k_strlen(args);
    if (len > CMD_BUF_SIZE - 1) len = CMD_BUF_SIZE - 1;
    sys_uart_write(args, len, session->caps);
    sys_uart_write("\r\n", 2, session->caps);
}

/* Run verified sandbox tool */
static void cmd_sandbox(const char *args, ai_session_t *session)
{
    if (!args || args[0] == '\0') {
        sys_uart_write("usage: sandbox <command> [args]\r\n", KSTRLEN("usage: sandbox <command> [args]\r\n"), session->caps);
        return;
    }
    sandbox_execute(&sb_ctx, args);
}

/* Run multi-step AIQL pipeline */
static void cmd_pipeline(const char *args, ai_session_t *session)
{
    if (!args || args[0] == '\0') {
        sys_uart_write("usage: pipeline <step1>; <step2>; ...\r\n", KSTRLEN("usage: pipeline <step1>; <step2>; ...\r\n"), session->caps);
        return;
    }
    sandbox_run_pipeline(&sb_ctx, args);
}

/* Master IRQ Enable */
static void cmd_irq(const char *args, ai_session_t *session)
{
    (void)args;
    irq_enable_in_cpu();
    sys_uart_write("IRQ dispatch enabled. PSTATE.I unmasked.\r\n", KSTRLEN("IRQ dispatch enabled. PSTATE.I unmasked.\r\n"), session->caps);
}

/* Bind an AI reflex to hardware (pipeline string → wrapped in aiql_program_t) */
static void cmd_irq_bind(const char *args, ai_session_t *session)
{
    if (!args || args[0] == '\0') {
        sys_uart_write("usage: irq_bind <num> <pipeline>\r\n", KSTRLEN("usage: irq_bind <num> <pipeline>\r\n"), session->caps);
        return;
    }

    uint32_t irq_num = 0;
    const char *p = args;
    while (*p >= '0' && *p <= '9') {
        irq_num = irq_num * 10 + (uint32_t)(*p - '0');
        p++;
    }
    while (*p == ' ') p++;

    /* Parse pipeline string into a single-pipeline aiql_program_t */
    static aiql_program_t bind_prog;
    k_memset(&bind_prog, 0, sizeof(bind_prog));

    if (!interpreter_parse_pipeline(p, &bind_prog.pipelines[0])) {
        sys_uart_write("Error parsing pipeline.\r\n", KSTRLEN("Error parsing pipeline.\r\n"), session->caps);
        return;
    }
    if (!verifier_check_pipeline(&bind_prog.pipelines[0], session->caps)) {
        sys_uart_write("Pipeline verification failed.\r\n", KSTRLEN("Pipeline verification failed.\r\n"), session->caps);
        return;
    }
    bind_prog.pipeline_count = 1;

    if (irq_register_pipeline(irq_num, &bind_prog, &sb_ctx)) {
        irq_enable(irq_num);
        sys_uart_write("IRQ bound and enabled.\r\n", KSTRLEN("IRQ bound and enabled.\r\n"), session->caps);
    } else {
        sys_uart_write("Registration failed (bad irq or slots full).\r\n", KSTRLEN("Registration failed (bad irq or slots full).\r\n"), session->caps);
    }
}

/* dag — Build DAG from a pipeline string, run scheduler, print plan */
static void cmd_dag(const char *args, ai_session_t *session)
{
    if (!args || args[0] == '\0') {
        sys_uart_write("usage: dag <step1>; <step2>; ...\r\n",
                       34, session->caps);
        return;
    }

    /* Stage 1: Parse pipeline text into pipeline_t */
    pipeline_t pipeline;
    if (!interpreter_parse_pipeline(args, &pipeline)) {
        sys_uart_write("[dag] pipeline parse error\r\n", 28, session->caps);
        return;
    }

    /* Stage 2: Verify all steps */
    if (!verifier_check_pipeline(&pipeline, session->caps)) {
        sys_uart_write("[dag] pipeline verification failed\r\n", 36, session->caps);
        return;
    }

    /* Stage 3: Build DAG — nodes are deduplicated via the global interner */
    kai_dag_t dag;
    if (!kai_dag_build_from_pipeline(&dag, &kai_intern, &pipeline)) {
        sys_uart_write("[dag] DAG construction failed\r\n", 31, session->caps);
        return;
    }

    sys_uart_write("[dag] nodes: ", 13, session->caps);
    sys_uart_hex64((uint64_t)dag.node_count, session->caps);
    sys_uart_write("\r\n", 2, session->caps);

    /* Stage 4: Cycle detection (safety gate before scheduling) */
    if (kai_dag_has_cycle(&dag)) {
        sys_uart_write("[dag] error: cycle detected\r\n", 29, session->caps);
        kai_dag_destroy(&dag);
        return;
    }

    /* Stage 5: Build execution schedule */
    kai_schedule_t schedule;
    kai_sched_result_t res = kai_scheduler_build(&dag, &schedule);
    if (res != KAI_SCHED_OK) {
        sys_uart_write("[dag] scheduler error: ", 23, session->caps);
        const char *msg = kai_sched_result_str(res);
        sys_uart_write(msg, k_strlen(msg), session->caps);
        sys_uart_write("\r\n", 2, session->caps);
        kai_dag_destroy(&dag);
        return;
    }

    /* Stage 6: Print the schedule */
    kai_scheduler_print(&schedule, session->caps);

    /* Stage 7: Print interner pool usage */
    kai_interner_stats(&kai_intern, session->caps);

    kai_dag_destroy(&dag);
}


/* ---- AIQL command --------------------------------------------------------
 * Accepts JSON inline on the command line, OR reads a large JSON payload
 * from UART when the args start with '{' but the shell buf was truncated.
 *
 * Two modes:
 *   aiql {"type":"Program",...}   -- JSON passed as arg (agent sends this)
 *   aiql                          -- prompt for multi-line JSON (interactive)
 *
 * The static json_buf lives here so it doesn't blow the stack.
 */
static char aiql_json_buf[AIQL_BUF_SIZE];

static void cmd_aiql(const char *args, ai_session_t *session)
{
    size_t len = 0;

    if (args && args[0] == '{') {
        /* ---- Inline mode: JSON was passed directly as the arg ----------- */
        len = k_strlen(args);
        if (len >= AIQL_BUF_SIZE) {
            uart_puts("[aiql] error: JSON too large (max ");
            /* print AIQL_BUF_SIZE-1 as decimal */
            char tmp[8]; uint32_t v=AIQL_BUF_SIZE-1, i=7; tmp[i]='\0';
            while (v && i>0) { tmp[--i]=(char)('0'+(int)(v%10)); v/=10; }
            uart_puts(tmp+i);
            uart_puts(" bytes)\r\n");
            return;
        }
        k_memset(aiql_json_buf, 0, sizeof(aiql_json_buf));
        for (size_t j=0; j<len; j++) aiql_json_buf[j] = args[j];
        aiql_json_buf[len] = '\0';
    } else {
        /* ---- Interactive mode: read JSON from UART until balanced '}' -- */
        uart_puts("[aiql] paste JSON then press Enter:\r\n");
        k_memset(aiql_json_buf, 0, sizeof(aiql_json_buf));
        len = 0;
        int depth = 0; bool started = false;

        while (len < AIQL_BUF_SIZE - 1U) {
            char c = uart_getc();
            if (c == '\r' || c == '\n') {
                if (started && depth == 0) break;   /* done */
                aiql_json_buf[len++] = ' ';           /* normalise newlines */
                continue;
            }
            if (c == '{') { depth++; started = true; }
            else if (c == '}') depth--;
            aiql_json_buf[len++] = c;
            uart_putc(c);                           /* echo back */
            if (started && depth == 0) break;       /* balanced */
        }
        aiql_json_buf[len] = '\0';
        uart_puts("\r\n");

        if (!started || len == 0) {
            uart_puts("[aiql] error: no JSON received\r\n");
            return;
        }
    }

    /* ---- Extract -------------------------------------------------------- */
    aiql_program_t prog;
    aiql_err_t err = aiql_extract(aiql_json_buf, len, &prog);
    if (err != AIQL_OK) {
        uart_puts("[aiql] extract error: ");
        uart_puts(aiql_err_str(err));
        uart_puts("\r\n");
        return;
    }

    /* ---- Execute -------------------------------------------------------- */
    aiql_execute_program(&prog, &sb_ctx, session->caps);
}


/* ---- aiql_bind — bind an AIQL JSON program to a hardware IRQ ------------ */
static void cmd_aiql_bind(const char *args, ai_session_t *session)
{
    if (!args || args[0] == '\0') {
        uart_puts("usage: aiql_bind <irq_num> <json>\r\n");
        return;
    }

    /* Parse IRQ number */
    uint32_t irq_num = 0;
    const char *p = args;
    while (*p >= '0' && *p <= '9') { irq_num = irq_num * 10 + (uint32_t)(*p - '0'); p++; }
    while (*p == ' ') p++;

    if (*p != '{') {
        uart_puts("[aiql_bind] expected JSON after irq number\r\n");
        return;
    }

    /* Extract AIQL */
    size_t json_len = k_strlen(p);
    if (json_len >= AIQL_BUF_SIZE) {
        uart_puts("[aiql_bind] JSON too large\r\n");
        return;
    }

    static aiql_program_t bind_prog;
    aiql_err_t err = aiql_extract(p, json_len, &bind_prog);
    if (err != AIQL_OK) {
        uart_puts("[aiql_bind] extract error: ");
        uart_puts(aiql_err_str(err));
        uart_puts("\r\n");
        return;
    }

    if (!irq_register_pipeline(irq_num, &bind_prog, &sb_ctx)) {
        uart_puts("[aiql_bind] registration failed (bad irq or slots full)\r\n");
        return;
    }

    irq_enable(irq_num);
    uart_puts("[aiql_bind] bound irq ");
    sys_uart_hex64((uint64_t)irq_num, session->caps);
    uart_puts(" goal=");
    uart_puts(bind_prog.goal[0] ? bind_prog.goal : "(none)");
    uart_puts("\r\n");
}

/* ---- timer_bind — arm the generic timer and bind an AIQL reflex --------- */
/*
 * Usage: timer_bind <interval_ms> <aiql_json>
 *
 * Configures the ARM EL1 physical timer (PPI IRQ 27) to fire every
 * interval_ms milliseconds. On each tick the AIQL program executes
 * and emits a RESPOND: packet. The agent bridge picks these up
 * asynchronously between its own command sends.
 *
 * Example:
 *   timer_bind 500 {"type":"Program","intent":{"goal":"heartbeat"},...}
 */
static void cmd_timer_bind(const char *args, ai_session_t *session)
{
    if (!args || args[0] == '\0') {
        uart_puts("usage: timer_bind <ms> <json>\r\n");
        return;
    }

    uint32_t ms = 0;
    const char *p = args;
    while (*p >= '0' && *p <= '9') { ms = ms * 10 + (uint32_t)(*p - '0'); p++; }
    while (*p == ' ') p++;

    if (ms == 0 || ms > 60000U) {
        uart_puts("[timer_bind] interval must be 1–60000 ms\r\n");
        return;
    }

    if (*p != '{') {
        uart_puts("[timer_bind] expected JSON after interval\r\n");
        return;
    }

    size_t json_len = k_strlen(p);
    if (json_len >= AIQL_BUF_SIZE) {
        uart_puts("[timer_bind] JSON too large\r\n");
        return;
    }

    static aiql_program_t timer_prog;
    aiql_err_t err = aiql_extract(p, json_len, &timer_prog);
    if (err != AIQL_OK) {
        uart_puts("[timer_bind] extract error: ");
        uart_puts(aiql_err_str(err));
        uart_puts("\r\n");
        return;
    }

    if (!irq_register_pipeline(IRQ_TIMER_PPI, &timer_prog, &sb_ctx)) {
        uart_puts("[timer_bind] registration failed\r\n");
        return;
    }

    timer_init(ms);
    irq_enable_in_cpu();   /* ensure CPU is accepting IRQs */

    uart_puts("[timer_bind] armed: every ");
    sys_uart_hex64((uint64_t)ms, session->caps);
    uart_puts("ms, goal=");
    uart_puts(timer_prog.goal[0] ? timer_prog.goal : "(none)");
    uart_puts("\r\n");
}

/* ---- irq_list — show active bindings ------------------------------------ */
static void cmd_irq_list(const char *args, ai_session_t *session)
{
    (void)args;
    irq_list(session->caps);
}

/* Help list */
static void cmd_help(const char *args, ai_session_t *session)
{
    (void)args;
    sys_uart_write("Available commands:\r\n", KSTRLEN("Available commands:\r\n"), session->caps);
    for (size_t i = 0; i < NUM_COMMANDS; i++) {
        sys_uart_write("  ", 2, session->caps);
        sys_uart_write(commands[i].name, k_strlen(commands[i].name), session->caps);
        sys_uart_write("\t- ", KSTRLEN("\t- "), session->caps);
        sys_uart_write(commands[i].help, k_strlen(commands[i].help), session->caps);
        sys_uart_write("\r\n", 2, session->caps);
    }
}

/* ---- Command dispatcher ---------------------------------------------- */
static void execute_command(const char *input, ai_session_t *session)
{
    sys_uart_write("\r\n", 2, session->caps);
    if (input[0] == '\0') return;

    for (size_t i = 0; i < NUM_COMMANDS; i++) {
        size_t name_len = k_strlen(commands[i].name);
        if ((k_strncmp(input, commands[i].name, name_len) == 0) &&
            (input[name_len] == '\0' || input[name_len] == ' ')) {
            const char *args = input + name_len;
            while (*args == ' ') args++;
            commands[i].handler(args, session);
            return;
        }
    }
    sys_uart_write("Unknown command.\r\n", KSTRLEN("Unknown command.\r\n"), session->caps);
}

/* ---- Entry point ----------------------------------------------------- */
void kernel_main(void)
{
    /* 1. UART Init */
    uart_init();

    /* 2. Memory whitelist — must come before any sandbox or verifier use */
    memory_init();

    /* 2. MMU Init */
    mmu_init();
    mmu_enable();

    /* 3. Exception Vectors (VBAR) */
    /* Sync with mmu.S: Using vbar_install to load kai_vector_table */
    vbar_install(); 

    /* 4. GIC Init */
    irq_init();

    /* 5. AI Session & Sandbox Preparation 
     * NOTE: If the kernel hangs here, the MMU mapping for the 
     * sandbox scratchpad (0x40400000) is likely missing.
     */
    ai_session_t session = { .caps = CAP_MMIO | CAP_READ_MEM | CAP_WRITE_MEM | CAP_SYSTEM };
    intent_object_t intent = {
        .caps = session.caps,
        .instruction_budget = 1000,
        .pipeline = NULL
    };
    sandbox_init(&sb_ctx, &intent);
    kai_interner_init(&kai_intern);

    /* 6. Welcome Banner */
    uart_puts(COL_BOLD COL_CYAN "========================\r\n");
    uart_puts("      K A I   O S      \r\n");
    uart_puts("========================\r\n" COL_RESET);

    uart_puts("EL: ");
    char el_char = '0' + (char)current_el();
    uart_putc(el_char);
    uart_puts("   |   Ready\r\n\r\n");

    uart_puts(PROMPT);

        /* 7. Main Shell Loop — now with real history + Up/Down arrows */
    char buf[CMD_BUF_SIZE];
    size_t index = 0U;

    while (true) {
        char c = uart_getc();

        if (c == '\r' || c == '\n') {
            buf[index] = '\0';
            if (index > 0U) {
                /* Save to history */
                k_strcpy(history[hist_idx], buf);
                hist_idx = (hist_idx + 1) % HISTORY_SIZE;
                if (hist_count < HISTORY_SIZE) hist_count++;

                execute_command(buf, &session);
                index = 0U;
                hist_view = -1;
            }
            uart_puts("\r\n");
            uart_puts(PROMPT);
        }
        else if ((c == '\x7F') || (c == '\x08')) {  /* backspace */
            if (index > 0U) {
                index--;
                uart_puts("\b \b");
            }
        }
        else if (c == '\x1B') {  /* ESC sequence (arrows) */
            uart_getc(); /* skip '[' */
            char dir = uart_getc();

            if (dir == 'A' && hist_count > 0) {  /* UP */
                if (hist_view < 0) hist_view = (int)hist_idx - 1;
                else hist_view = (hist_view - 1 + HISTORY_SIZE) % HISTORY_SIZE;

                k_strcpy(buf, history[hist_view]);
                index = k_strlen(buf);

                /* Redraw cleanly with ANSI (this is what fixes the backspace bug) */
                uart_puts("\r" PROMPT);
                uart_puts(buf);
                uart_puts("\033[K");  /* clear to end of line */
            }
            else if (dir == 'B' && hist_count > 0) {  /* DOWN */
                if (hist_view >= 0) {
                    hist_view = (hist_view + 1) % HISTORY_SIZE;
                    if (hist_view == (int)hist_idx) {
                        hist_view = -1;
                        buf[0] = '\0';
                        index = 0U;
                    } else {
                        k_strcpy(buf, history[hist_view]);
                        index = k_strlen(buf);
                    }

                    uart_puts("\r" PROMPT);
                    uart_puts(buf);
                    uart_puts("\033[K");
                }
            }
        }
        else if (is_printable(c)) {
            if (index < (CMD_BUF_SIZE - 1U)) {
                buf[index++] = c;
                uart_putc(c);
            } else if (index == (CMD_BUF_SIZE - 1U)) {
                /* Buffer full: if this is an aiql command, switch to the
                 * wide JSON buffer and keep reading until braces balance. */
                buf[index] = '\0';
                if (k_strncmp(buf, "aiql ", 5) == 0) {
                    /* Copy what we have so far into the JSON buf */
                    size_t json_start = 5; /* skip "aiql " */
                    size_t ji = 0;
                    int depth = 0; bool started = false;
                    /* Seed from existing buf */
                    for (size_t si = json_start;
                         si < CMD_BUF_SIZE - 1U && ji < AIQL_BUF_SIZE - 1U;
                         si++) {
                        char bc = buf[si];
                        if (bc == '{') { depth++; started = true; }
                        else if (bc == '}') depth--;
                        aiql_json_buf[ji++] = bc;
                    }
                    /* Now keep reading from UART */
                    uart_putc(c); /* echo the char that triggered overflow */
                    if (c == '{') { depth++; started = true; }
                    else if (c == '}') depth--;
                    if (ji < AIQL_BUF_SIZE - 1U) aiql_json_buf[ji++] = c;

                    while (ji < AIQL_BUF_SIZE - 1U) {
                        char jc = uart_getc();
                        if (jc == '\r' || jc == '\n') {
                            if (started && depth == 0) break;
                            aiql_json_buf[ji++] = ' ';
                            continue;
                        }
                        if (jc == '\x7F' || jc == '\x08') {
                            if (ji > 0) { ji--; uart_puts("\b \b"); }
                            continue;
                        }
                        if (jc == '{') { depth++; started = true; }
                        else if (jc == '}') depth--;
                        aiql_json_buf[ji++] = jc;
                        uart_putc(jc);
                        if (started && depth == 0) break;
                    }
                    aiql_json_buf[ji] = '\0';
                    uart_puts("\r\n");

                    /* Execute directly — bypass normal dispatch */
                    if (started && ji > 0) {
                        aiql_program_t prog;
                        aiql_err_t err = aiql_extract(aiql_json_buf, ji, &prog);
                        if (err != AIQL_OK) {
                            uart_puts("[aiql] extract error: ");
                            uart_puts(aiql_err_str(err));
                            uart_puts("\r\n");
                        } else {
                            aiql_execute_program(&prog, &sb_ctx, session.caps);
                        }
                    }
                    index = 0U;
                    uart_puts(PROMPT);
                }
                /* else: non-aiql command, just drop the char silently */
            }
        }
    }
}