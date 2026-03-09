#!/usr/bin/env python3
"""
tools/aiql_to_kai.py — AIQL AST → KAI Pipeline Compiler
=========================================================
Translates a full AIQL JSON program (as defined in ast/aiql_schema.json)
into one or more KAI pipeline strings that can be sent to the kernel.

This is the missing bridge between the AIQL specification and the KAI
kernel runtime. The LLM generates AIQL JSON; this compiler translates it.

Supported AIQL node types:
  Program              — top-level container with optional intent
  PipelineStatement    — maps to a `pipeline` kernel command
  Operation            — maps to a pipeline step with output binding
  CallStatement        — call_type drives opcode selection:
                           "function" → echo + write (kernel-side effect)
                           "classifier" / "llm" / "model" → echo stub
                           (full model dispatch is a future kai_executor layer)
  ConditionalStatement — maps to OP_IF with then/else step counts
  BinaryExpression     — maps to pipeline_cond_t (left op right)
  Variable             — referenced by name in pipeline step args
  Literal              — inline value in condition operands
  ReturnStatement      — maps to echo (returns named var to UART)

Intentionally NOT supported (require hosted OS):
  LoadStatement        — needs filesystem / network
  CallStatement visualize — needs display

Intent handling:
  Program.intent.goal        → emitted as an echo at pipeline start
  Program.intent.success_metric → evaluated post-run by the agent bridge
  Program.intent.fallback    → emitted as a comment / echo if metric fails

Usage:
  python3 tools/aiql_to_kai.py program.json
  python3 tools/aiql_to_kai.py program.json --emit-commands
  python3 tools/aiql_to_kai.py --validate program.json
  echo '{"type":"Program",...}' | python3 tools/aiql_to_kai.py -
"""

import sys
import json
import argparse
from typing import Any, Dict, List, Optional, Tuple


# ── AIQL node types ──────────────────────────────────────────────────────────

NODE_PROGRAM        = "Program"
NODE_PIPELINE       = "PipelineStatement"
NODE_OPERATION      = "Operation"
NODE_CALL           = "CallStatement"
NODE_CONDITIONAL    = "ConditionalStatement"
NODE_RETURN         = "ReturnStatement"
NODE_LOAD           = "LoadStatement"
NODE_BINARY_EXPR    = "BinaryExpression"
NODE_VARIABLE       = "Variable"
NODE_LITERAL        = "Literal"

CALL_LLM            = "llm"
CALL_CLASSIFIER     = "classifier"
CALL_MODEL          = "model"
CALL_FUNCTION       = "function"
CALL_VISUALIZE      = "visualize"

KAI_MAX_STEPS       = 16
KAI_ARG_MAX_LEN     = 31   # one less than SANDBOX_ARG_MAX_LEN for safety
KAI_MAX_SLEEP_MS    = 10000


# ── Exceptions ───────────────────────────────────────────────────────────────

class AIQLCompileError(Exception):
    pass


# ── Step representation ───────────────────────────────────────────────────────

class KaiStep:
    """One step in a KAI pipeline."""
    def __init__(self, opcode: str, args: List[str] = None, output_var: str = ""):
        self.opcode     = opcode
        self.args       = args or []
        self.output_var = output_var

    def to_str(self) -> str:
        parts = [self.opcode] + self.args
        s = " ".join(parts)
        if self.output_var:
            s += f" -> {self.output_var}"
        return s


class KaiIfStep:
    """Compiled OP_IF with flattened then/else branches."""
    def __init__(self, left: str, op: str, right: str,
                 then_steps: List[KaiStep], else_steps: List[KaiStep]):
        self.left       = left
        self.op         = op
        self.right      = right
        self.then_steps = then_steps
        self.else_steps = else_steps

    def to_str(self) -> str:
        n = len(self.then_steps)
        m = len(self.else_steps)
        cond = f"if {self.left} {self.op} {self.right}"
        if m > 0:
            cond += f" -> then:{n} else:{m}"
        else:
            cond += f" -> then:{n}"
        parts = [cond]
        for s in self.then_steps:
            parts.append(s.to_str())
        for s in self.else_steps:
            parts.append(s.to_str())
        return "; ".join(parts)


AnyStep = "KaiStep | KaiIfStep"


# ── Compiler ─────────────────────────────────────────────────────────────────

class AIQLCompiler:
    """
    Walks an AIQL Program AST and emits a list of KAI pipeline command strings.

    One pipeline string is emitted per PipelineStatement in the program body.
    ConditionalStatements at the top level are wrapped in a single-step pipeline.
    """

    def __init__(self, warn_unsupported: bool = True):
        self.warn_unsupported = warn_unsupported
        self.warnings: List[str] = []

    def _warn(self, msg: str):
        self.warnings.append(msg)
        if self.warn_unsupported:
            print(f"  warning: {msg}", file=sys.stderr)

    # ── Top level ─────────────────────────────────────────────────────────

    def compile_program(self, program: Dict) -> List[str]:
        """
        Compile a full AIQL Program to a list of KAI pipeline strings.
        Each string is ready to send as: `pipeline <string>`
        """
        if program.get("type") != NODE_PROGRAM:
            raise AIQLCompileError(f"Expected Program node, got: {program.get('type')}")

        pipelines: List[str] = []

        # Emit intent goal as an echo pipeline prefix
        intent = program.get("intent")
        if intent and isinstance(intent, dict):
            goal = intent.get("goal", "")
            if goal:
                safe_goal = self._safe_echo(f"GOAL: {goal}")
                pipelines.append(safe_goal)

        # Compile body statements
        for stmt in program.get("body", []):
            result = self._compile_statement(stmt)
            if result:
                pipelines.extend(result)

        return pipelines

    def _compile_statement(self, node: Dict) -> List[str]:
        t = node.get("type")

        if t == NODE_PIPELINE:
            return [self._compile_pipeline(node)]

        elif t == NODE_CONDITIONAL:
            # Top-level conditional → wrap in a pipeline
            steps = self._compile_conditional(node)
            return [self._steps_to_pipeline(steps)]

        elif t == NODE_CALL:
            steps = self._compile_call(node)
            return [self._steps_to_pipeline(steps)]

        elif t == NODE_RETURN:
            steps = self._compile_return(node)
            return [self._steps_to_pipeline(steps)]

        elif t == NODE_LOAD:
            self._warn(f"LoadStatement is not supported on bare metal (skipped)")
            return []

        else:
            self._warn(f"Unknown top-level statement type '{t}' (skipped)")
            return []

    # ── PipelineStatement ─────────────────────────────────────────────────

    def _compile_pipeline(self, node: Dict) -> str:
        """Compile a PipelineStatement to a flat pipeline string."""
        steps: List[AnyStep] = []

        # Optional pipeline-level intent as echo
        intent = node.get("intent")
        if intent and isinstance(intent, str) and intent.strip():
            steps.append(KaiStep("echo", [self._truncate(f"[{intent}]", KAI_ARG_MAX_LEN)]))

        for step_node in node.get("steps", []):
            compiled = self._compile_step(step_node)
            steps.extend(compiled)

        return self._steps_to_pipeline(steps)

    def _compile_step(self, node: Dict) -> List[AnyStep]:
        t = node.get("type")

        if t == NODE_OPERATION:
            return self._compile_operation(node)
        elif t == NODE_CALL:
            return self._compile_call(node)
        elif t == NODE_CONDITIONAL:
            return [self._compile_conditional(node)]
        elif t == NODE_RETURN:
            return self._compile_return(node)
        else:
            self._warn(f"Unknown step type '{t}' (skipped)")
            return []

    # ── Operation ─────────────────────────────────────────────────────────

    def _compile_operation(self, node: Dict) -> List[KaiStep]:
        """
        AIQL Operation → KAI step.

        The operation 'name' and 'params' drive opcode selection:
          FeatureEngineering / transform ops → echo (stub; future: write params)
          read / sensor ops                  → read (if address in params)
          write / actuator ops               → write
          info / introspect                  → info / introspect
          sleep / delay / wait               → sleep
          Otherwise                          → echo describing the operation
        """
        name   = node.get("name", "").lower()
        output = node.get("output", "")
        params = node.get("params", {})

        step = self._operation_name_to_step(name, params, output)
        return [step]

    def _operation_name_to_step(self, name: str, params: dict, output: str) -> KaiStep:
        """Heuristic mapping from AIQL operation names to KAI opcodes."""

        # Sensor / memory read
        if any(k in name for k in ("read", "sensor", "fetch", "load", "input")):
            addr = params.get("address", params.get("addr", None))
            length = params.get("length", params.get("len", 4))
            if addr:
                return KaiStep("read", [str(addr), str(length)], output)
            # No address — fall through to echo stub

        # Actuator / scratch write
        if any(k in name for k in ("write", "set", "actuate", "output", "motor", "drive")):
            offset = str(params.get("offset", 0))
            value  = str(params.get("value", params.get("val", "0x00")))
            return KaiStep("write", [offset, value], output)

        # Sleep / delay / timing
        if any(k in name for k in ("sleep", "delay", "wait", "pause", "timer")):
            ms = str(params.get("ms", params.get("duration", params.get("milliseconds", 100))))
            return KaiStep("sleep", [ms], output)

        # Memory / system info
        if any(k in name for k in ("info", "meminfo", "memory_info")):
            return KaiStep("info", [], output)

        # Hardware introspection
        if any(k in name for k in ("introspect", "mmio", "hardware_map")):
            return KaiStep("introspect", [], output)

        # Exception level
        if any(k in name for k in ("el", "exception_level", "privilege")):
            return KaiStep("el", [], output)

        # Capability query
        if any(k in name for k in ("caps", "capabilities", "permissions")):
            return KaiStep("caps", [], output)

        # Feature engineering, normalisation, transforms — echo stub
        label = self._truncate(name.replace("_", " "), KAI_ARG_MAX_LEN)
        self._warn(f"Operation '{name}' has no direct KAI opcode mapping — emitting echo stub")
        return KaiStep("echo", [label], output)

    # ── CallStatement ─────────────────────────────────────────────────────

    def _compile_call(self, node: Dict) -> List[KaiStep]:
        """
        AIQL CallStatement → KAI steps.

        call_type mapping:
          "function"   → maps by action name (same as Operation heuristic)
          "classifier" → echo stub with cost annotation (future: kai_executor dispatch)
          "llm"        → echo stub with cost annotation (future: kai_executor dispatch)
          "model"      → same as classifier
          "visualize"  → unsupported, skipped
        """
        call_type = node.get("call_type", "function")
        action    = node.get("action", "unknown")
        outputs   = node.get("outputs", [])
        params    = node.get("params", {})
        output    = outputs[0] if outputs else ""

        if call_type == CALL_VISUALIZE:
            self._warn(f"CallStatement visualize '{action}' not supported on bare metal (skipped)")
            return []

        if call_type == CALL_FUNCTION:
            step = self._operation_name_to_step(action.lower(), params, output)
            return [step]

        if call_type in (CALL_LLM, CALL_CLASSIFIER, CALL_MODEL):
            # Stub: echo the call intent. Future: kai_executor will dispatch
            # to a registered model handler via OP_MODEL_CALL (not yet implemented).
            label = self._truncate(f"[{call_type}:{action}]", KAI_ARG_MAX_LEN)
            self._warn(
                f"CallStatement {call_type} '{action}' emitted as echo stub. "
                f"Full dispatch requires kai_executor (future layer)."
            )
            return [KaiStep("echo", [label], output)]

        self._warn(f"Unknown call_type '{call_type}' (skipped)")
        return []

    # ── ConditionalStatement ──────────────────────────────────────────────

    def _compile_conditional(self, node: Dict) -> KaiIfStep:
        """
        AIQL ConditionalStatement → KaiIfStep.
        Recursively flattens then_body and else_body.
        """
        condition  = node.get("condition", {})
        then_body  = node.get("then_body", [])
        else_body  = node.get("else_body", [])

        left, op, right = self._compile_condition(condition)

        then_steps = self._compile_body(then_body)
        else_steps = self._compile_body(else_body)

        # KAI limitation: then/else branches must be flat KaiStep lists
        # (no nested IfSteps). Flatten nested conditionals to echo stubs.
        then_flat = self._flatten_steps(then_steps, "then")
        else_flat = self._flatten_steps(else_steps, "else")

        # KAI requires then_count >= 1
        if not then_flat:
            then_flat = [KaiStep("nop")]

        return KaiIfStep(left, op, right, then_flat, else_flat)

    def _compile_condition(self, node: Dict) -> Tuple[str, str, str]:
        """Extract (left, op, right) strings from a BinaryExpression."""
        if node.get("type") != NODE_BINARY_EXPR:
            raise AIQLCompileError(f"Expected BinaryExpression in condition, got: {node.get('type')}")

        op    = node.get("operator", "==")
        left  = self._compile_operand(node.get("left",  {}))
        right = self._compile_operand(node.get("right", {}))

        return left, op, right

    def _compile_operand(self, node: Dict) -> str:
        t = node.get("type")
        if t == NODE_VARIABLE:
            return node.get("name", "unknown")
        elif t == NODE_LITERAL:
            return str(node.get("value", "0"))
        else:
            raise AIQLCompileError(f"Unknown operand type: {t}")

    def _compile_body(self, body: List[Dict]) -> List[AnyStep]:
        steps = []
        for stmt in body:
            steps.extend(self._compile_step(stmt))
        return steps

    def _flatten_steps(self, steps: List[AnyStep], branch: str) -> List[KaiStep]:
        """
        KAI OP_IF branches must be flat lists of KaiStep (no nested OP_IF).
        Nested conditionals are replaced with an echo warning stub.
        """
        flat = []
        for s in steps:
            if isinstance(s, KaiIfStep):
                self._warn(
                    f"Nested conditional in {branch}-branch flattened to echo stub. "
                    f"KAI OP_IF does not support nested branches."
                )
                flat.append(KaiStep("echo", [self._truncate(f"[nested-if:{branch}]", KAI_ARG_MAX_LEN)]))
            else:
                flat.append(s)
        return flat

    # ── ReturnStatement ───────────────────────────────────────────────────

    def _compile_return(self, node: Dict) -> List[KaiStep]:
        """
        AIQL ReturnStatement → echo the variable name.
        The agent bridge reads UART output to capture the result.
        """
        var = node.get("variable", "result")
        return [KaiStep("echo", [self._truncate(f"RETURN:{var}", KAI_ARG_MAX_LEN)])]

    # ── Helpers ───────────────────────────────────────────────────────────

    def _steps_to_pipeline(self, steps: List[AnyStep]) -> str:
        if not steps:
            return "nop"
        parts = [s.to_str() for s in steps]
        pipeline = "; ".join(parts)
        # Validate step count
        step_count = len(parts)
        if step_count > KAI_MAX_STEPS:
            self._warn(
                f"Pipeline has {step_count} steps, exceeding KAI limit of {KAI_MAX_STEPS}. "
                f"Truncating to first {KAI_MAX_STEPS} steps."
            )
            parts = parts[:KAI_MAX_STEPS]
            pipeline = "; ".join(parts)
        return pipeline

    def _safe_echo(self, text: str) -> str:
        return f"echo {self._truncate(text, KAI_ARG_MAX_LEN)}"

    def _truncate(self, s: str, max_len: int) -> str:
        s = s.replace(";", ",").replace("\n", " ").strip()
        if len(s) > max_len:
            s = s[:max_len - 1] + "~"
        return s


# ── Intent evaluator (host-side) ──────────────────────────────────────────────

def evaluate_success_metric(metric: str, context: Dict[str, Any]) -> bool:
    """
    Evaluate an AIQL success_metric expression against a context dict.

    The metric is a simple comparison expression like:
      "confidence_score >= 0.7"
      "result != null"
      "step_count < 5"

    Context keys come from the agent's observation of UART output.
    Returns True if the metric passes, False otherwise.
    """
    if not metric:
        return True

    # Simple substitution: replace variable names with their context values
    expr = metric
    for key, val in sorted(context.items(), key=lambda x: -len(x[0])):
        expr = expr.replace(key, str(val))

    try:
        return bool(eval(expr, {"__builtins__": {}}))
    except Exception:
        return False


# ── Validator ──────────────────────────────────────────────────────────────

def validate_aiql(program: Dict) -> List[str]:
    """
    Light structural validation of an AIQL program before compilation.
    Returns a list of error strings (empty = valid).
    """
    errors = []

    if program.get("type") != NODE_PROGRAM:
        errors.append(f"Root node must be Program, got: {program.get('type')}")
        return errors

    body = program.get("body")
    if not isinstance(body, list):
        errors.append("Program.body must be an array")
        return errors

    if not body:
        errors.append("Program.body is empty")

    intent = program.get("intent")
    if isinstance(intent, dict):
        if "goal" not in intent:
            errors.append("Program.intent object must have a 'goal' field")

    for i, stmt in enumerate(body):
        if not isinstance(stmt, dict):
            errors.append(f"Body[{i}] is not an object")
            continue
        t = stmt.get("type")
        if t == NODE_PIPELINE:
            steps = stmt.get("steps", [])
            if not isinstance(steps, list):
                errors.append(f"Body[{i}] PipelineStatement.steps must be an array")
        elif t == NODE_CONDITIONAL:
            if "condition" not in stmt:
                errors.append(f"Body[{i}] ConditionalStatement missing 'condition'")
            if "then_body" not in stmt:
                errors.append(f"Body[{i}] ConditionalStatement missing 'then_body'")

    return errors


# ── CLI ───────────────────────────────────────────────────────────────────────

def main():
    parser = argparse.ArgumentParser(
        description="AIQL JSON → KAI pipeline compiler",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Examples:
  python3 tools/aiql_to_kai.py ast/example_pipeline.json
  python3 tools/aiql_to_kai.py ast/example_pipeline.json --emit-commands
  python3 tools/aiql_to_kai.py --validate ast/example_pipeline.json
  echo '{"type":"Program","body":[...]}' | python3 tools/aiql_to_kai.py -
        """
    )
    parser.add_argument("input", nargs="?", default="-",
                        help="AIQL JSON file path, or - for stdin")
    parser.add_argument("--emit-commands", action="store_true",
                        help="Prefix each pipeline with 'pipeline ' for direct KAI shell use")
    parser.add_argument("--validate", action="store_true",
                        help="Validate the AIQL program and exit (no compilation)")
    parser.add_argument("--no-warn", action="store_true",
                        help="Suppress compiler warnings")
    args = parser.parse_args()

    # Load input
    if args.input == "-":
        source = sys.stdin.read()
    else:
        try:
            with open(args.input) as f:
                source = f.read()
        except FileNotFoundError:
            print(f"Error: file not found: {args.input}", file=sys.stderr)
            sys.exit(1)

    try:
        program = json.loads(source)
    except json.JSONDecodeError as e:
        print(f"Error: invalid JSON: {e}", file=sys.stderr)
        sys.exit(1)

    # Validate
    errors = validate_aiql(program)
    if errors:
        for e in errors:
            print(f"  error: {e}", file=sys.stderr)
        sys.exit(1)

    if args.validate:
        print("AIQL program is valid.")
        return

    # Compile
    compiler = AIQLCompiler(warn_unsupported=not args.no_warn)
    try:
        pipelines = compiler.compile_program(program)
    except AIQLCompileError as e:
        print(f"Compile error: {e}", file=sys.stderr)
        sys.exit(1)

    # Output
    for pipeline in pipelines:
        if args.emit_commands:
            print(f"pipeline {pipeline}")
        else:
            print(pipeline)


if __name__ == "__main__":
    main()