#!/usr/bin/env python3
"""
kai_agent.py — KAI OS LLM Agent Bridge (AIQL-native)
======================================================
Full autonomous agent loop. The LLM generates structured AIQL JSON programs,
the aiql_to_kai compiler translates them to KAI pipeline strings, and the
kernel verifier + DAG + scheduler + executor runs them. Output is fed back
to the LLM for reasoning and replanning.

Architecture:
    User goal (natural language)
          ↓
    LLM generates AIQL JSON program  ← structured, auditable intent
          ↓
    aiql_to_kai compiler             ← AIQL AST → KAI pipeline strings
          ↓
    Unix socket → QEMU serial → KAI kernel
          ↓  verifier + DAG + scheduler + executor
    UART output
          ↓
    LLM observes output, updates context, replans or declares done
          ↓
    Loop (max --max-steps iterations)

Usage:
    # Terminal 1:
    make run-agent

    # Terminal 2:
    export ANTHROPIC_API_KEY=sk-...
    python3 tools/kai_agent.py --goal "inspect hardware and report system state"
    python3 tools/kai_agent.py --goal "write 0xAB to scratch offset 3 and verify it"
    python3 tools/kai_agent.py --provider openai --goal "run a timed actuator sequence"
    python3 tools/kai_agent.py --dump-aiql --goal "..."   # save AIQL programs to disk

Requirements:
    pip install anthropic   (for Claude)
    pip install openai      (for OpenAI)
"""

import os
import sys
import threading
import json
import time
import socket
import argparse
import textwrap
import re
from pathlib import Path

# Import the AIQL→KAI compiler from the same tools/ directory
sys.path.insert(0, str(Path(__file__).parent))
from aiql_to_kai import validate_aiql, evaluate_success_metric


# ── Colour helpers ──────────────────────────────────────────────────────────

class C:
    RESET   = "\033[0m"
    BOLD    = "\033[1m"
    DIM     = "\033[2m"
    KERNEL  = "\033[36m"
    LLM     = "\033[32m"
    THINK   = "\033[33m"
    PLAN    = "\033[35m"
    AIQL    = "\033[34m"
    ERROR   = "\033[31m"
    SUCCESS = "\033[32m"

USE_COLOR = True

def c(text, *codes):
    if not USE_COLOR:
        return text
    return "".join(codes) + text + C.RESET

def log_kernel(line):
    print(c(f"  [kernel] {line}", C.KERNEL))

def log_llm(label, content):
    print(c(f"  [llm]    {label}: ", C.LLM + C.BOLD) + content)

def log_think(thought):
    for line in thought.strip().splitlines():
        print(c(f"           {line}", C.THINK))

def log_aiql(program: dict):
    """Pretty-print an AIQL program summary."""
    intent = program.get("intent", {})
    goal = intent.get("goal", "") if isinstance(intent, dict) else str(intent)
    body_count = len(program.get("body", []))
    print(c(f"  [aiql]   goal: {goal} | {body_count} statement(s)", C.AIQL + C.BOLD))

def log_pipeline(p):
    print(c(f"  [pipe]   ", C.PLAN + C.BOLD) + c(p, C.PLAN))

def log_step(n, total):
    bar = "─" * 58
    print()
    print(c(bar, C.DIM))
    print(c(f"  Step {n}/{total}", C.BOLD))
    print(c(bar, C.DIM))

def log_error(msg):
    print(c(f"  [error]  {msg}", C.ERROR))

def log_success(msg):
    print(c(f"\n  [done]   {msg}", C.SUCCESS + C.BOLD))


# ── KAI socket bridge ───────────────────────────────────────────────────────

PROMPT  = "kai# "
CHUNK   = 4096
_ANSI   = re.compile(r"\033\[[0-9;]*[mK]")

def _strip_ansi(s):
    return _ANSI.sub("", s)

def connect_socket(path, retries=30, interval=0.5):
    sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    for attempt in range(retries):
        try:
            sock.connect(path)
            sock.settimeout(0.1)
            return sock
        except (ConnectionRefusedError, FileNotFoundError):
            if attempt == 0:
                print(c("  Waiting for kernel", C.DIM), end="", flush=True)
            else:
                print(c(".", C.DIM), end="", flush=True)
            time.sleep(interval)
    print()
    raise RuntimeError(f"Could not connect to {path} after {retries} attempts")

def recv_until_prompt(sock, timeout=15.0):
    buf = b""
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            chunk = sock.recv(CHUNK)
            if chunk:
                buf += chunk
                if _strip_ansi(buf.decode("utf-8", errors="replace")).rstrip().endswith(PROMPT.rstrip()):
                    break
            else:
                break
        except socket.timeout:
            if _strip_ansi(buf.decode("utf-8", errors="replace")).rstrip().endswith(PROMPT.rstrip()):
                break
    return buf.decode("utf-8", errors="replace")

def parse_respond(lines: list) -> dict | None:
    """
    Scan output lines for a RESPOND:{...} packet.
    Returns parsed dict if found, None otherwise.
    """
    import json as _json
    for line in lines:
        if line.startswith("RESPOND:"):
            try:
                return _json.loads(line[8:])
            except _json.JSONDecodeError:
                pass
    return None


# ── Reflex listener ─────────────────────────────────────────────────────────
# Runs in a background thread. Drains RESPOND: packets emitted by
# IRQ-triggered AIQL programs (timer reflexes, sensor events) and
# queues them for the main agent loop to observe.

_reflex_queue: list = []
_reflex_lock = threading.Lock()
_reflex_active = False

def _reflex_reader(sock):
    """Background thread: read lines, queue any RESPOND: packets."""
    buf = b""
    while _reflex_active:
        try:
            chunk = sock.recv(256)
            if not chunk:
                break
            buf += chunk
            while b"\n" in buf or b"\r" in buf:
                line, _, buf = buf.partition(b"\n")
                line = _strip_ansi(line.decode("utf-8", errors="replace")).strip()
                if line.startswith("RESPOND:"):
                    try:
                        import json as _j
                        data = _j.loads(line[8:])
                        with _reflex_lock:
                            _reflex_queue.append(data)
                        print(c(f"\n  [reflex]  {line}", C.AIQL + C.BOLD), flush=True)
                    except Exception:
                        pass
        except Exception:
            break

def start_reflex_listener(sock):
    global _reflex_active
    _reflex_active = True
    t = threading.Thread(target=_reflex_reader, args=(sock,), daemon=True)
    t.start()
    return t

def stop_reflex_listener():
    global _reflex_active
    _reflex_active = False

def drain_reflexes() -> list:
    """Return and clear all queued reflex packets."""
    with _reflex_lock:
        packets = list(_reflex_queue)
        _reflex_queue.clear()
    return packets


def drain(sock):
    sock.settimeout(0.05)
    try:
        while sock.recv(CHUNK):
            pass
    except socket.timeout:
        pass
    sock.settimeout(0.1)

def send_command(sock, cmd) -> list:
    drain(sock)
    sock.sendall((cmd + "\r\n").encode())
    raw = recv_until_prompt(sock, timeout=20.0)
    lines = []
    for line in raw.splitlines():
        line = _strip_ansi(line).strip()
        if not line or line == cmd.strip():
            continue
        if line == PROMPT.strip() or line.endswith(PROMPT.strip()):
            tail = line[:-len(PROMPT.strip())].strip()
            if tail:
                lines.append(tail)
            continue
        lines.append(line)
    return lines

def wait_for_boot(sock) -> list:
    print(c("  Waiting for kernel boot", C.DIM), end="", flush=True)
    raw = recv_until_prompt(sock, timeout=20.0)
    print(c(" ready.", C.DIM))
    return [_strip_ansi(l).strip() for l in raw.splitlines()
            if l.strip() and PROMPT.strip() not in l]


# ── System prompt ───────────────────────────────────────────────────────────

SYSTEM_PROMPT = textwrap.dedent("""
You are the planning and reasoning layer for KAI OS — a bare-metal AI kernel
running on AArch64. You control the kernel by generating AIQL programs that
get compiled and executed on the hardware.

═══════════════════════════════════════════════
WHAT YOU GENERATE
═══════════════════════════════════════════════

You must emit a single valid AIQL JSON Program. The bridge compiles it to
KAI pipeline strings automatically. You never write pipeline strings directly.

AIQL Program structure:
{
  "type": "Program",
  "intent": {
    "goal": "short identifier",
    "description": "what this program does",
    "success_metric": "optional expression e.g. caps_read == true",
    "fallback": "optional fallback action name"
  },
  "body": [ ...statements... ]
}

═══════════════════════════════════════════════
STATEMENT TYPES
═══════════════════════════════════════════════

PipelineStatement — sequence of operations:
{
  "type": "PipelineStatement",
  "variable": "output_name",
  "source": "input_name",
  "intent": "what this pipeline does",
  "steps": [ ...step nodes... ]
}

Step types inside a pipeline:

  Operation:
  {
    "type": "Operation",
    "name": "IntrospectHardware",
    "inputs": [],
    "output": "hw_map",
    "params": {}
  }

  CallStatement:
  {
    "type": "CallStatement",
    "call_type": "function",   // function | classifier | llm | model
    "action": "read_sensor",
    "inputs": [],
    "outputs": ["sensor_val"],
    "params": { "address": "0x40200000", "length": 4 }
  }

ConditionalStatement:
{
  "type": "ConditionalStatement",
  "condition": {
    "type": "BinaryExpression",
    "operator": ">=",          // == != < > <= >=
    "left":  { "type": "Variable", "name": "sensor_val" },
    "right": { "type": "Literal",  "value": 10 }
  },
  "then_body": [ ...statements... ],
  "else_body":  [ ...statements... ]
}

ReturnStatement:
{ "type": "ReturnStatement", "variable": "result_name" }

═══════════════════════════════════════════════
OPERATION NAME → KAI OPCODE MAPPING
═══════════════════════════════════════════════

The compiler maps Operation/CallStatement names to KAI opcodes:

  Names containing: read, sensor, fetch → OP_READ (needs params.address)
  Names containing: write, set, actuate, motor → OP_WRITE (needs params.offset, params.value)
  Names containing: sleep, delay, wait  → OP_SLEEP (needs params.ms)
  Names containing: info, meminfo       → OP_INFO
  Names containing: introspect, mmio    → OP_INTROSPECT
  Names containing: el, exception_level → OP_EL
  Names containing: caps, capabilities  → OP_CAPS
  Anything else                         → echo stub (use for reasoning steps)

CallStatement with call_type "llm" or "classifier" emits an echo stub.

═══════════════════════════════════════════════
TIMER REFLEXES (IRQ-triggered autonomous pipelines)
═══════════════════════════════════════════════

You can register an AIQL program to fire autonomously on a hardware timer.
Use the `timer_bind` shell command as a single-step pipeline Operation:

  Operation name: "BindTimerReflex"
  params: { "interval_ms": 1000, "goal": "heartbeat" }

The bridge will send:
  timer_bind 1000 {"type":"Program","intent":{"goal":"heartbeat"},...}

The kernel will then fire that AIQL program every 1000ms via IRQ 27.
Each firing emits a RESPOND: packet you'll see as [reflex] in the output.

Use timer reflexes to:
  - Poll hardware state periodically without agent commands
  - Set up a "watchdog" that reports anomalies autonomously
  - Establish a heartbeat so the agent knows the kernel is alive

ALWAYS end your final pipeline with a respond operation:
{
  "type": "Operation",
  "name": "RespondWithResult",
  "params": { "goal": "your_goal_label" }
}

This emits a RESPOND:{...} packet the bridge parses directly — you get
typed values (caps, el, vars) instead of raw text to interpret.
Full model dispatch is a future kai_executor layer.

═══════════════════════════════════════════════
MEMORY CONSTRAINTS (CRITICAL)
═══════════════════════════════════════════════

- OP_READ only works on WHITELISTED addresses (BSS, stack, scratch buffer)
- Always run an IntrospectHardware or GetMemoryInfo operation FIRST
- The scratch buffer address is reported by OP_INFO as "sandbox_scratch"
- DO NOT guess addresses — they will fail kernel verification
- Safe first pipelines: introspect, el, caps, info (no addresses needed)

═══════════════════════════════════════════════
RESPONSE FORMAT (STRICT)
═══════════════════════════════════════════════

Respond with a single JSON object. No prose, no markdown fences.

While working toward the goal:
{
  "thought": "reasoning about current state and next action",
  "aiql": { ...full AIQL Program object... },
  "done": false,
  "conclusion": null
}

When the goal is fully achieved:
{
  "thought": "goal achieved because ...",
  "aiql": null,
  "done": true,
  "conclusion": "one sentence summary of what was accomplished"
}

If the goal is impossible given constraints:
{
  "thought": "cannot proceed because ...",
  "aiql": null,
  "done": true,
  "conclusion": "FAILED: reason"
}

═══════════════════════════════════════════════
AGENT LOOP CONTEXT
═══════════════════════════════════════════════

Each user message contains the output from the last AIQL program execution.
Use the full history to build understanding of kernel state.

Start with system introspection. Prefer pipelines with multiple steps over
many single-step programs. Never repeat an operation you already have output for.
""").strip()


# ── LLM clients ─────────────────────────────────────────────────────────────

def call_claude(messages, model):
    try:
        import anthropic
    except ImportError:
        raise RuntimeError("pip install anthropic")
    api_key = os.environ.get("ANTHROPIC_API_KEY")
    if not api_key:
        raise RuntimeError("ANTHROPIC_API_KEY not set")
    client = anthropic.Anthropic(api_key=api_key)
    resp = client.messages.create(
        model=model, max_tokens=2048,
        system=SYSTEM_PROMPT, messages=messages,
    )
    return resp.content[0].text

def call_openai(messages, model):
    try:
        import openai
    except ImportError:
        raise RuntimeError("pip install openai")
    api_key = os.environ.get("OPENAI_API_KEY")
    if not api_key:
        raise RuntimeError("OPENAI_API_KEY not set")
    client = openai.OpenAI(api_key=api_key)
    full = [{"role": "system", "content": SYSTEM_PROMPT}] + messages
    resp = client.chat.completions.create(model=model, max_tokens=2048, messages=full)
    return resp.choices[0].message.content

def call_llm(messages, provider, model):
    if provider == "anthropic":
        return call_claude(messages, model)
    elif provider == "openai":
        return call_openai(messages, model)
    raise ValueError(f"Unknown provider: {provider}")

def parse_response(text):
    text = text.strip()
    text = re.sub(r"^```(?:json)?\s*", "", text)
    text = re.sub(r"\s*```$", "", text)
    try:
        return json.loads(text)
    except json.JSONDecodeError:
        match = re.search(r"\{.*\}", text, re.DOTALL)
        if match:
            try:
                return json.loads(match.group(0))
            except json.JSONDecodeError:
                pass
    raise ValueError(f"Could not parse LLM response as JSON.\n\nRaw response:\n{text}")


# ── Agent loop ───────────────────────────────────────────────────────────────

def run_agent(goal, sock, provider, model, max_steps, dump_aiql, verbose):
    print()
    print(c("═" * 60, C.DIM))
    print(c("  KAI OS — AIQL Agent", C.BOLD))
    print(c("═" * 60, C.DIM))
    print(c(f"  Goal     : ", C.DIM) + goal)
    print(c(f"  Provider : {provider} / {model}", C.DIM))
    print(c(f"  Budget   : {max_steps} steps", C.DIM))
    print(c("═" * 60, C.DIM))

    boot_lines = wait_for_boot(sock)
    if verbose:
        for line in boot_lines:
            log_kernel(line)

    # Start background reflex listener for IRQ-fired RESPOND: packets
    start_reflex_listener(sock)

    messages = [
        {
            "role": "user",
            "content": (
                f"Goal: {goal}\n\n"
                "The KAI kernel is booted and ready. "
                "Generate your first AIQL program. "
                "Start with system introspection to understand the hardware state."
            )
        }
    ]

    step = 0
    while step < max_steps:
        step += 1
        log_step(step, max_steps)

        # ── LLM call ───────────────────────────────────────────────────────
        print(c("  Thinking...", C.DIM), end="", flush=True)
        try:
            raw = call_llm(messages, provider, model)
        except Exception as e:
            log_error(f"LLM call failed: {e}")
            break
        print(c(" done.", C.DIM))

        try:
            resp = parse_response(raw)
        except ValueError as e:
            log_error(str(e))
            messages.append({"role": "assistant", "content": raw})
            messages.append({
                "role": "user",
                "content": "ERROR: Your response was not valid JSON. "
                           "Respond with only a JSON object matching the required format."
            })
            continue

        # ── Reasoning ──────────────────────────────────────────────────────
        thought = resp.get("thought", "")
        if thought:
            log_think(thought)

        # ── Completion check ───────────────────────────────────────────────
        if resp.get("done"):
            log_success(resp.get("conclusion", "Goal complete."))
            print()
            return True

        aiql_program = resp.get("aiql")
        if not aiql_program:
            log_error("LLM returned no AIQL program and done=false. Stopping.")
            break

        # ── Validate AIQL ──────────────────────────────────────────────────
        errors = validate_aiql(aiql_program)
        if errors:
            log_error(f"AIQL validation failed: {errors}")
            messages.append({"role": "assistant", "content": raw})
            messages.append({
                "role": "user",
                "content": f"ERROR: Your AIQL program failed validation:\n" +
                           "\n".join(f"  - {e}" for e in errors) +
                           "\n\nFix the program and try again."
            })
            continue

        log_aiql(aiql_program)

        # ── Optionally dump AIQL to disk ───────────────────────────────────
        if dump_aiql:
            path = f"/tmp/kai_aiql_step_{step:02d}.json"
            with open(path, "w") as f:
                json.dump(aiql_program, f, indent=2)
            print(c(f"  [aiql]   saved to {path}", C.DIM))

        # ── Send AIQL JSON directly to kernel `aiql` command ─────────────
        aiql_json = json.dumps(aiql_program, separators=(',', ':'))
        cmd = f"aiql {aiql_json}"
        log_aiql(aiql_program)
        output_lines = send_command(sock, cmd)
        for line in output_lines:
            log_kernel(line)
        output_text = "\n".join(output_lines) if output_lines else "(no output)"

        # ── Parse RESPOND: packet if present ──────────────────────────────
        respond_data = parse_respond(output_lines)
        if respond_data:
            print(c(f"  [respond] {json.dumps(respond_data)}", C.AIQL))

        # ── Check success_metric ───────────────────────────────────────────
        intent = aiql_program.get("intent", {})
        if isinstance(intent, dict):
            metric = intent.get("success_metric", "")
            if metric:
                context = {"step_count": step}
                if respond_data:
                    context.update(respond_data)
                    context.update(respond_data.get("vars", {}))
                passed = evaluate_success_metric(metric, context)
                status = "PASS" if passed else "FAIL"
                print(c(f"  [metric] {metric} → {status}", C.AIQL))

        # ── Feed result back ───────────────────────────────────────────────
        # ── Collect any IRQ-fired reflex packets ──────────────────────────
        reflex_packets = drain_reflexes()
        if reflex_packets:
            print(c(f"  [reflexes] {len(reflex_packets)} packet(s) from IRQ handlers", C.AIQL))

        messages.append({"role": "assistant", "content": raw})

        if respond_data:
            # Structured path: LLM gets typed values, not raw text
            reflex_str = ""
            if reflex_packets:
                reflex_str = f"\n\nIRQ reflex packets received ({len(reflex_packets)}):\n"
                reflex_str += "\n".join(json.dumps(r, indent=2) for r in reflex_packets)
            feedback = (
                f"AIQL program executed successfully.\n\n"
                f"Structured result (RESPOND packet):\n"
                f"{json.dumps(respond_data, indent=2)}\n\n"
                f"Raw output (for context):\n{output_text}"
                f"{reflex_str}\n\n"
                f"Steps remaining: {max_steps - step}\n\n"
                "The RESPOND packet gives you typed values from the kernel. "
                "Use them to determine if the goal is achieved or plan the next step."
            )
        else:
            # Unstructured path: raw UART text
            feedback = (
                f"AIQL program executed. No RESPOND packet emitted.\n\n"
                f"Kernel output:\n{output_text}\n\n"
                f"Steps remaining: {max_steps - step}\n\n"
                "Tip: add a respond operation as the final step of your pipeline "
                "to emit a structured result packet for reliable parsing."
            )

        messages.append({"role": "user", "content": feedback})

    print()
    log_error(f"Step budget exhausted after {step} steps.")
    return False


# ── Entry point ──────────────────────────────────────────────────────────────

DEFAULT_MODELS = {
    "anthropic": "claude-opus-4-5",
    "openai":    "gpt-4o",
}

def main():
    global USE_COLOR
    parser = argparse.ArgumentParser(
        description="KAI OS AIQL Agent Bridge",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=textwrap.dedent("""
            Examples:
              make run-agent
              python3 tools/kai_agent.py --goal "inspect hardware and report system state"
              python3 tools/kai_agent.py --goal "write 0xAB to scratch offset 3 and verify"
              python3 tools/kai_agent.py --dump-aiql --goal "run a timed pipeline"
              python3 tools/kai_agent.py --provider openai --goal "query capabilities"
        """)
    )
    parser.add_argument("--goal",       required=True)
    parser.add_argument("--provider",   default="anthropic", choices=["anthropic", "openai"])
    parser.add_argument("--model",      default=None)
    parser.add_argument("--socket",     default="/tmp/kai.sock")
    parser.add_argument("--max-steps",  type=int, default=10)
    parser.add_argument("--dump-aiql",  action="store_true",
                        help="Save each AIQL program to /tmp/kai_aiql_step_N.json")
    parser.add_argument("--no-color",   action="store_true")
    parser.add_argument("--verbose",    action="store_true")
    args = parser.parse_args()

    if args.no_color:
        USE_COLOR = False

    model = args.model or DEFAULT_MODELS[args.provider]

    try:
        sock = connect_socket(args.socket)
        print(c("\n  Connected to KAI kernel.\n", C.LLM))
        success = run_agent(
            goal=args.goal,
            sock=sock,
            provider=args.provider,
            model=model,
            max_steps=args.max_steps,
            dump_aiql=args.dump_aiql,
            verbose=args.verbose,
        )
        stop_reflex_listener()
        sock.close()
        sys.exit(0 if success else 1)
    except RuntimeError as e:
        log_error(str(e))
        sys.exit(1)
    except KeyboardInterrupt:
        print(c("\n\n  Agent interrupted.", C.DIM))
        sys.exit(0)

if __name__ == "__main__":
    main()