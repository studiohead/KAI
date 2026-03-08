#!/usr/bin/env python3
"""
tools/kai_compiler.py — KAI Script Compiler

Compiles a human-friendly .kai script into the semicolon-separated pipeline
string format accepted by KAI OS's `pipeline` command.

Usage:
    python3 tools/kai_compiler.py <script.kai>
    python3 tools/kai_compiler.py <script.kai> --run        # print as shell cmd
    python3 tools/kai_compiler.py --introspect              # show all opcodes

KAI Script syntax (.kai files):
    # Comment lines start with #
    <opcode> [args] [-> varname]     # regular step
    if <left> <op> <right>           # conditional (must be followed by then/else blocks)
        then:
            <steps>
        else:
            <steps>                  # else block is optional

Supported opcodes:
    nop                       — no operation
    echo <text>               — print text to UART
    read <addr> <len>         — read bytes from whitelisted address
    write <offset> <value>    — write byte to scratch buffer
    info                      — print memory layout
    el                        — print exception level
    caps                      — print capability mask
    sleep <ms>                — busy-wait for N milliseconds (0–10000)
    introspect                — print MMIO address map
    wait_event                — yield until next hardware event

Examples:

  # sensor_check.kai — read a distance sensor and echo the result
  read 0x40000000 4 -> dist
  echo sensor read complete

  # obstacle_avoid.kai — conditional motor response
  read 0x40000000 4 -> front
  if front < 10
      then:
          write 0 0x02
          echo reversing
      else:
          write 0 0x01
          echo forward

  # timing.kai — two-step with a sleep between
  echo starting sequence
  sleep 500
  echo sequence complete
"""

import sys
import re
import argparse
from dataclasses import dataclass, field
from typing import List, Optional

# ---- Token types -----------------------------------------------------------

VALID_OPCODES = {
    "nop", "echo", "read", "write", "info", "el",
    "caps", "sleep", "introspect", "wait_event", "if"
}

COMPARE_OPS = {"==", "!=", "<", ">", "<=", ">="}

# ---- AST nodes -------------------------------------------------------------

@dataclass
class Step:
    """A single pipeline step: opcode + args + optional output binding."""
    opcode: str
    args: List[str] = field(default_factory=list)
    output_var: Optional[str] = None

    def to_pipeline_str(self) -> str:
        parts = [self.opcode] + self.args
        s = " ".join(parts)
        if self.output_var:
            s += f" -> {self.output_var}"
        return s


@dataclass
class IfStep:
    """An if/then/else conditional block."""
    left: str
    op: str
    right: str
    then_steps: List["AnyStep"] = field(default_factory=list)
    else_steps: List["AnyStep"] = field(default_factory=list)

    def to_pipeline_str(self) -> str:
        """
        Flatten the if/then/else into the KAI pipeline wire format:
            if <left> <op> <right> -> then:<N> else:<M>; <then steps>; <else steps>
        """
        n = len(self.then_steps)
        m = len(self.else_steps)

        cond = f"if {self.left} {self.op} {self.right}"
        if m > 0:
            cond += f" -> then:{n} else:{m}"
        else:
            cond += f" -> then:{n}"

        parts = [cond]
        for step in self.then_steps:
            parts.append(step.to_pipeline_str())
        for step in self.else_steps:
            parts.append(step.to_pipeline_str())
        return "; ".join(parts)


AnyStep = "Step | IfStep"

# ---- Parser ----------------------------------------------------------------

class KAIParser:
    """
    Simple line-by-line parser for .kai scripts.

    Indentation is used only inside then:/else: blocks.
    Lines outside blocks are flat steps.
    """

    def __init__(self, source: str):
        # Strip comments and blank lines, keep track of indentation
        self.lines = []
        for raw in source.splitlines():
            stripped = raw.rstrip()
            # Remove inline comments
            if "#" in stripped:
                stripped = stripped[:stripped.index("#")].rstrip()
            self.lines.append(stripped)
        self.pos = 0

    def _current(self) -> Optional[str]:
        while self.pos < len(self.lines):
            line = self.lines[self.pos]
            if line.strip():
                return line
            self.pos += 1
        return None

    def _consume(self) -> str:
        line = self.lines[self.pos]
        self.pos += 1
        return line

    def _indent(self, line: str) -> int:
        return len(line) - len(line.lstrip())

    def parse(self) -> List:
        """Parse all top-level steps."""
        steps = []
        while self._current() is not None:
            step = self._parse_step(expected_indent=0)
            if step:
                steps.append(step)
        return steps

    def _parse_step(self, expected_indent: int):
        line = self._current()
        if line is None:
            return None

        indent = self._indent(line)
        if indent < expected_indent:
            return None  # Back-indent — caller handles it

        self._consume()
        tokens = line.split()
        if not tokens:
            return None

        opcode = tokens[0].lower()

        if opcode not in VALID_OPCODES:
            raise SyntaxError(
                f"Unknown opcode '{opcode}' on line {self.pos}. "
                f"Valid opcodes: {sorted(VALID_OPCODES)}"
            )

        if opcode == "if":
            return self._parse_if(tokens, indent)

        return self._parse_regular(tokens)

    def _parse_regular(self, tokens: List[str]) -> Step:
        opcode = tokens[0].lower()
        args = []
        output_var = None

        i = 1
        while i < len(tokens):
            if tokens[i] == "->":
                if i + 1 < len(tokens):
                    output_var = tokens[i + 1]
                i += 2
                continue
            args.append(tokens[i])
            i += 1

        # echo: rejoin args as a single quoted string for UART output
        if opcode == "echo" and args:
            args = [" ".join(args)]

        return Step(opcode=opcode, args=args, output_var=output_var)

    def _parse_if(self, tokens: List[str], base_indent: int) -> IfStep:
        """
        Parse:
            if <left> <op> <right>
                then:
                    <steps>
                else:           ← optional
                    <steps>
        """
        if len(tokens) < 4:
            raise SyntaxError(
                f"'if' requires: if <left> <op> <right>, got: {' '.join(tokens)}"
            )

        left  = tokens[1]
        op    = tokens[2]
        right = tokens[3]

        if op not in COMPARE_OPS:
            raise SyntaxError(
                f"Unknown comparison operator '{op}'. "
                f"Valid: {sorted(COMPARE_OPS)}"
            )

        then_steps = []
        else_steps = []

        # Expect then: block
        line = self._current()
        if line and line.strip().lower().rstrip(":") == "then":
            self._consume()
            block_indent = base_indent + 4  # expect 4-space indent
            # Detect actual indent from first step
            next_line = self._current()
            if next_line:
                block_indent = self._indent(next_line)
            then_steps = self._parse_block(block_indent)

        # Optional else: block
        line = self._current()
        if line and line.strip().lower().rstrip(":") == "else":
            self._consume()
            next_line = self._current()
            block_indent = base_indent + 4
            if next_line:
                block_indent = self._indent(next_line)
            else_steps = self._parse_block(block_indent)

        return IfStep(left=left, op=op, right=right,
                      then_steps=then_steps, else_steps=else_steps)

    def _parse_block(self, block_indent: int) -> List:
        """Parse steps at the given indentation level."""
        steps = []
        while True:
            line = self._current()
            if line is None:
                break
            indent = self._indent(line)
            if indent < block_indent:
                break
            step = self._parse_step(expected_indent=block_indent)
            if step:
                steps.append(step)
        return steps


# ---- Compiler --------------------------------------------------------------

class KAICompiler:
    """Turns a parsed step list into a KAI pipeline string."""

    def compile(self, steps: List) -> str:
        parts = []
        for step in steps:
            parts.append(step.to_pipeline_str())
        return "; ".join(parts)


# ---- Validator (light pre-check before sending to KAI) --------------------

def validate(pipeline_str: str) -> List[str]:
    """
    Return a list of warnings for common mistakes.
    The KAI verifier on the kernel side is authoritative — this is
    a convenience check to catch obvious errors before flashing.
    """
    warnings = []
    steps = [s.strip() for s in pipeline_str.split(";") if s.strip()]

    if len(steps) > 16:
        warnings.append(
            f"Pipeline has {len(steps)} steps — KAI limit is 16 (PIPELINE_MAX_STEPS)."
        )

    for i, step in enumerate(steps):
        tokens = step.split()
        if not tokens:
            continue
        opcode = tokens[0]

        if opcode == "sleep":
            if len(tokens) < 2:
                warnings.append(f"Step {i}: sleep requires a millisecond argument.")
            else:
                try:
                    ms = int(tokens[1])
                    if ms > 10000:
                        warnings.append(
                            f"Step {i}: sleep {ms}ms exceeds KAI maximum (10000ms)."
                        )
                except ValueError:
                    warnings.append(
                        f"Step {i}: sleep argument '{tokens[1]}' is not an integer."
                    )

        if opcode == "echo":
            text = " ".join(tokens[1:])
            if len(text) >= 32:
                warnings.append(
                    f"Step {i}: echo text length {len(text)} may exceed "
                    f"SANDBOX_ARG_MAX_LEN (32)."
                )

        if opcode == "read":
            if len(tokens) < 3:
                warnings.append(f"Step {i}: read requires address and length.")

        if opcode == "write":
            if len(tokens) < 3:
                warnings.append(f"Step {i}: write requires offset and value.")

    return warnings


# ---- Introspection ---------------------------------------------------------

def print_opcodes():
    opcode_help = {
        "nop":        "No operation",
        "echo":       "echo <text>            — print text to UART",
        "read":       "read <addr> <len>      — read bytes from whitelisted address",
        "write":      "write <offset> <value> — write byte to scratch buffer",
        "info":       "info                   — print BSS/stack memory layout",
        "el":         "el                     — print current exception level",
        "caps":       "caps                   — print session capability mask",
        "sleep":      "sleep <ms>             — busy-wait N milliseconds (0–10000)",
        "introspect": "introspect             — print MMIO address map",
        "wait_event": "wait_event             — yield to hardware (WFE stub)",
        "if":         "if <l> <op> <r>        — conditional branch (==, !=, <, >, <=, >=)",
    }
    print("KAI OS opcodes:")
    for op, desc in sorted(opcode_help.items()):
        print(f"  {desc}")


# ---- Main ------------------------------------------------------------------

def main():
    parser = argparse.ArgumentParser(
        description="KAI Script Compiler — compile .kai scripts to KAI pipeline strings"
    )
    parser.add_argument("script", nargs="?", help=".kai source file")
    parser.add_argument("--run", action="store_true",
                        help="Emit as a KAI shell command (pipeline <...>)")
    parser.add_argument("--introspect", action="store_true",
                        help="Print all available opcodes and exit")
    args = parser.parse_args()

    if args.introspect:
        print_opcodes()
        return

    if not args.script:
        parser.print_help()
        sys.exit(1)

    try:
        with open(args.script) as f:
            source = f.read()
    except FileNotFoundError:
        print(f"Error: file not found: {args.script}", file=sys.stderr)
        sys.exit(1)

    try:
        kai_parser = KAIParser(source)
        steps = kai_parser.parse()
    except SyntaxError as e:
        print(f"Syntax error: {e}", file=sys.stderr)
        sys.exit(1)

    if not steps:
        print("Warning: empty script produces no pipeline.", file=sys.stderr)
        sys.exit(0)

    compiler = KAICompiler()
    pipeline_str = compiler.compile(steps)

    # Validation warnings
    warnings = validate(pipeline_str)
    for w in warnings:
        print(f"Warning: {w}", file=sys.stderr)

    if args.run:
        print(f"pipeline {pipeline_str}")
    else:
        print(pipeline_str)


if __name__ == "__main__":
    main()
