# KAI — Kernel AI Operating System

<i>How do you give an AI model meaningful control over a system without giving it arbitrary code execution?</i>
 
KAI is a formally verified, capability-gated, pipeline-based command language that sits between the AI and the hardware. The AI can only do what the verifier permits, and the verifier runs before any execution. This is architecturally cleaner than sandboxing approaches that restrict after the fact.

---

## Features

- **Bare-metal AArch64 boot** — custom `boot.S` entry point, BSS zeroing, stack init
- **PL011 UART driver** — blocking I/O with explicit hardware initialisation
- **Capability-gated syscalls** — every kernel operation checks a per-session bitmask
- **Memory region whitelist** — all memory reads validated against linker-defined regions
- **AI session model** — each shell session carries a capability mask controlling access
- **Lightweight sandbox** — parse → verify → execute pipeline for AI tool calls
  - Text-based tool call parser
  - Pre-execution AST verifier (opcode whitelist, argument validation, address checks)
  - Hard instruction count limit per invocation
  - Isolated scratch buffer — sandboxed writes never reach kernel memory
- **AIQL pipeline engine** — multi-step pipeline execution derived from the AIQL/PIQL AST schema
  - Semicolon-separated steps with `->` output variable bindings
  - Named variable store persisting across pipeline steps
  - Conditional branching via `OP_IF` (maps to AIQL `ConditionalStatement`)
  - Full pipeline pre-verification before any step executes

---

## Project Layout

```
kai_os/
├── include/
│   └── kernel/
│       ├── memory.h        # Memory region whitelist and sys_mem_* declarations
│       ├── mmio.h          # Typed MMIO read/write helpers
│       ├── sandbox.h       # Sandbox types, opcodes, pipeline structs, public interface
│       ├── string.h        # Freestanding string utilities
│       ├── syscall.h       # Capability flags and syscall declarations
│       └── uart.h          # UART public interface
├── src/
│   ├── arch/
│   │   └── aarch64/
│   │       └── boot.S      # Entry point, BSS zero, stack init, EL detection
│   ├── lib/
│   │   └── string.c        # k_strcmp, k_strncmp, k_strlen, k_memset
│   ├── sandbox/
│   │   ├── interpreter.c   # Tool call + pipeline parser, variable store, opcode dispatcher
│   │   ├── sandbox.c       # Sandbox init, single-shot execute, pipeline run, result strings
│   │   └── verifier.c      # Pre-execution AST and pipeline validation
│   ├── kernel.c            # kernel_main, AI session, command shell
│   ├── memory.c            # Memory regions, sys_mem_info, sys_mem_read
│   ├── syscall.c           # sys_uart_write, sys_uart_hex64
│   └── uart.c              # PL011 UART driver
├── scripts/
│   └── linker.ld           # Memory layout, guard page, stack region
├── Makefile
└── README.md
```

---

## Requirements

| Tool                   | macOS                              | Arch Linux                           |
|------------------------|------------------------------------|--------------------------------------|
| `aarch64-elf-gcc`      | `brew install aarch64-elf-gcc`     | `pacman -S aarch64-elf-gcc`          |
| `aarch64-elf-binutils` | included with above                | `pacman -S aarch64-elf-binutils`     |
| `qemu-system-aarch64`  | `brew install qemu`                | `pacman -S qemu-system-aarch64`      |

---

## Build & Run

```sh
make            # build/kernel.elf + build/kernel.img
make run        # launch QEMU in current terminal
make size       # print section sizes
make clean      # remove build/
```

---

## Shell Commands

| Command               | Description                                             |
|-----------------------|---------------------------------------------------------|
| `help`                | List all available commands                             |
| `clear`               | Clear the terminal screen                               |
| `el`                  | Print current exception level (EL1–EL3)                 |
| `hex`                 | Print an example 64-bit hex value                       |
| `mem`                 | Print BSS and stack boundary addresses                  |
| `echo <text>`         | Echo text back to the terminal                          |
| `sandbox <call>`      | Parse, verify, and execute a single sandboxed tool call |
| `pipeline <steps>`    | Parse, verify, and execute a multi-step AIQL pipeline   |

---

## Sandbox Tool Calls

The sandbox accepts a simple text format: `<opcode> [arg0] [arg1]`

| Tool call                | Description                                          |
|--------------------------|------------------------------------------------------|
| `nop`                    | No operation                                         |
| `echo <text>`            | Print text to UART (requires `CAP_MMIO`)             |
| `read <addr> <len>`      | Read bytes from a whitelisted address                |
| `write <offset> <value>` | Write one byte to the scratch buffer                 |
| `info`                   | Print BSS and stack addresses                        |
| `el`                     | Print current exception level                        |
| `caps`                   | Print current session capability mask                |

**Single-shot examples:**
```
sandbox echo hello
sandbox read 0x40000000 8
sandbox write 0 0xFF
sandbox info
sandbox caps
```

---

## AIQL Pipeline Execution

Pipelines are semicolon-separated sequences of tool calls. Each step can bind
its result to a named variable using `->`, which subsequent steps can reference.
The entire pipeline is verified before any step executes.

**Format:**
```
pipeline <step1>; <step2>; <step3>
pipeline <step> -> <varname>; <next step>
pipeline if <left> <op> <right> -> then:<N> else:<M>; <then steps...>; <else steps...>
```

**Supported operators for `if`:** `==` `!=` `<` `>` `<=` `>=`

**Pipeline examples:**
```
pipeline el -> level; caps; echo done
pipeline read 0x40000000 4 -> data; echo read complete
pipeline if 1 == 1 -> then:1 else:1; echo condition true; echo condition false
pipeline nop; echo step one; echo step two; echo step three
```

---

## Capability Flags

| Flag            | Value  | Grants                                        |
|-----------------|--------|-----------------------------------------------|
| `CAP_NONE`      | `0x00` | No capabilities                               |
| `CAP_READ_MEM`  | `0x01` | Read whitelisted memory regions               |
| `CAP_WRITE_MEM` | `0x02` | Write to sandbox scratch buffer               |
| `CAP_MMIO`      | `0x04` | UART output and MMIO access                   |
| `CAP_SYSTEM`    | `0x08` | Privileged system operations                  |

The default session starts with `CAP_MMIO | CAP_READ_MEM`.

---

## Safety Design

- **BSS zeroing** uses pointer comparison (`__bss_start` vs `__bss_end`) — no word-count drift
- **Stack guard page** — 4 KB unmapped region between `.bss` and stack catches overflows
- **Linker symbols** use `PROVIDE()` so empty `.bss` never causes a link error
- **`/DISCARD/`** strips `.comment`, `.eh_frame`, and ARM unwind tables from the binary
- **Address whitelisting** — `sys_mem_read` validates the entire requested range fits within
  a whitelisted region before copying a single byte
- **Sandbox verifier** runs before any execution — opcode whitelist, argument count check,
  capability check, and address range validation all happen before `interpreter_exec` is called
- **Pipeline pre-verification** — the entire pipeline is verified before any step executes;
  a bad step mid-sequence never causes partial execution
- **Scratch buffer isolation** — `OP_WRITE` can only target `ctx->scratch`, never kernel memory
- **Instruction limit** — `SANDBOX_MAX_INSNS` (64) prevents runaway sandbox or pipeline execution
- **Non-printable input** rejected at the shell level before entering any command handler
- **Variable store bounded** — `VAR_STORE_SIZE` (8) caps memory used by pipeline variable bindings

---

## AIQL Integration

The pipeline engine is derived from the [AIQL/PIQL](https://github.com/studiohead/AIQL) project.
The following AST node types from the AIQL JSON schema are implemented natively in C:

| AIQL Schema Type       | KAI OS Implementation                         |
|------------------------|-----------------------------------------------|
| `PipelineStatement`    | `pipeline_t` — array of `pipeline_node_t`     |
| `Operation`            | `pipeline_node_t` with `output_var` binding   |
| `ConditionalStatement` | `OP_IF` with `then_count` / `else_count`      |
| `BinaryExpression`     | `pipeline_cond_t` with `cmp_op_t`             |
| `Variable`             | `var_entry_t` in `var_store_t`                |
| `Literal`              | `uint64_t` inline in `operand_t`              |

The following AIQL types are intentionally omitted as they require hosted
environment capabilities (network, filesystem, display) unavailable at EL1:

- `CallStatement` (model / visualize)
- `LoadStatement`

---

## Author

**Stephen Johnny Davis**

---

## License

MIT License

Copyright (c) 2026 Stephen Johnny Davis

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
