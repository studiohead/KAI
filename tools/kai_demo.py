#!/usr/bin/env python3
"""
kai_demo.py — KAI OS AI Demo Script

Usage:
    # Terminal 1 — start the kernel:
    qemu-system-aarch64 -M virt -cpu max -nographic \
        -kernel build/kernel.elf \
        -serial unix:/tmp/kai.sock,server,nowait \
        -monitor none 2>/dev/null

    # Terminal 2 — run the demo:
    python3 tools/kai_demo.py
"""

import socket
import time
import sys
import argparse
import textwrap

class C:
    RESET  = "\033[0m"
    BOLD   = "\033[1m"
    KERNEL = "\033[36m"
    AI     = "\033[32m"
    THINK  = "\033[33m"
    ERROR  = "\033[31m"
    DIM    = "\033[2m"

USE_COLOR = True

def colorize(text, color):
    return f"{color}{text}{C.RESET}" if USE_COLOR else text

def print_kernel(line):
    print(colorize(f"  [kernel] {line}", C.KERNEL))

def print_ai(label, cmd):
    tag = colorize("[  ai  ]", C.AI + C.BOLD)
    print(f"{tag} {colorize(label + ':', C.AI)} {cmd}")

def print_think(thought):
    print(colorize(f"           ↳ {thought}", C.THINK))

def print_section(title):
    bar = "─" * 60
    print()
    print(colorize(bar, C.DIM))
    print(colorize(f"  {title}", C.BOLD))
    print(colorize(bar, C.DIM))

def print_error(msg):
    print(colorize(f"  [error] {msg}", C.ERROR))

PROMPT = "m4-kernel# "
CHUNK  = 4096

def connect(path, retries=20, interval=0.5):
    sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    for attempt in range(retries):
        try:
            sock.connect(path)
            sock.settimeout(0.1)
            return sock
        except (ConnectionRefusedError, FileNotFoundError):
            if attempt == 0:
                print(colorize("  Waiting for kernel", C.DIM), end="", flush=True)
            else:
                print(colorize(".", C.DIM), end="", flush=True)
            time.sleep(interval)
    print()
    raise RuntimeError(f"Could not connect to {path} after {retries} attempts")

def recv_all(sock, timeout=8.0):
    """Read all available bytes until the socket goes quiet for 0.1s."""
    buf = b""
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            chunk = sock.recv(CHUNK)
            if chunk:
                buf += chunk
                # If we see the prompt, we're done
                if buf.decode("utf-8", errors="replace").endswith(PROMPT):
                    break
            else:
                break
        except socket.timeout:
            # No data for 0.1s — check if we have a complete prompt
            decoded = buf.decode("utf-8", errors="replace")
            if decoded.strip().endswith(PROMPT.strip()):
                break
            # Otherwise keep waiting if we have partial data
            if buf and time.time() < deadline:
                continue
            break
    return buf.decode("utf-8", errors="replace")

def send_command(sock, cmd):
    """Send a command, wait for prompt, return response lines (minus echo)."""
    # Drain anything stale in the buffer first
    sock.settimeout(0.05)
    try:
        while sock.recv(CHUNK):
            pass
    except socket.timeout:
        pass

    sock.settimeout(0.1)
    sock.sendall((cmd + "\r\n").encode())

    # Read until we get the prompt back
    sock.settimeout(0.1)
    response = recv_all(sock, timeout=10.0)

    # Clean up: split, strip ANSI, remove echoed command and bare prompt
    lines = []
    for line in response.splitlines():
        line = line.strip()
        if not line:
            continue
        if line == cmd.strip():
            continue
        if line == PROMPT.strip():
            continue
        if line.endswith(PROMPT.strip()):
            # Strip trailing prompt from last line
            line = line[: -len(PROMPT.strip())].strip()
            if line:
                lines.append(line)
            continue
        lines.append(line)
    return lines

def wait_for_boot(sock):
    """Wait for the kernel to boot and show the first prompt."""
    print(colorize("  Waiting for kernel boot...", C.DIM))
    sock.settimeout(0.1)
    output = recv_all(sock, timeout=15.0)
    lines = []
    for line in output.splitlines():
        line = line.strip()
        if line and line != PROMPT.strip():
            lines.append(line)
    return lines

DEMO_STEPS = [
    (
        "Step 1 — System Introspection",
        "query hardware map",
        "sandbox introspect",
        "First I need to understand what hardware I can access."
    ),
    (
        "Step 2 — Check Privilege Level",
        "verify execution context",
        "sandbox el",
        "Confirming I am running inside the kernel at EL1."
    ),
    (
        "Step 3 — Query Capabilities",
        "read capability mask",
        "sandbox caps",
        "Checking what operations the kernel has granted this session."
    ),
    (
        "Step 4 — Write Actuator State",
        "set motor forward",
        "sandbox write 0 0x01",
        "Writing 0x01 to scratch offset 0 — forward motor command."
    ),
    (
        "Step 5 — Verify Write",
        "read back scratch",
        "sandbox read 0x40200000 4",
        "Reading scratch base to confirm the write was committed."
    ),
    (
        "Step 6 — Timed Sequence",
        "timed motor sequence",
        "pipeline sleep 200; write 0 0x02; echo reversing; sleep 200; write 0 0x01; echo forward",
        "Running a timed manoeuvre: reverse for 200ms then resume forward."
    ),
    (
        "Step 7 — Sense and Report",
        "read environment",
        "pipeline read 0x40200000 4; echo sense complete",
        "Reading the scratch region and confirming the sensing cycle."
    ),
    (
        "Step 8 — Idle",
        "complete cycle",
        "pipeline sleep 100; echo cycle complete",
        "Cycle complete. Sleeping briefly before next loop."
    ),
]

def run_demo(sock, step_delay):
    print_section("KAI OS — AI Demo")
    boot_lines = wait_for_boot(sock)
    for line in boot_lines:
        print_kernel(line)

    print()
    print(colorize(
        "  KAI OS is live. Beginning AI agent demonstration.\n"
        "  Every command passes through the hardware-enforced verifier.\n"
        "  The MMU bounds all sandbox access to 0x40200000.\n",
        C.DIM
    ))

    for (section, label, cmd, reasoning) in DEMO_STEPS:
        print_section(section)
        print_think(reasoning)
        time.sleep(step_delay * 0.5)

        print_ai(label, cmd)
        time.sleep(step_delay * 0.2)

        lines = send_command(sock, cmd)
        for line in lines:
            print_kernel(line)

        time.sleep(step_delay * 0.3)

    print_section("Demo Complete")
    print(colorize(textwrap.dedent("""
        The AI agent completed 8 steps:
          • Queried hardware layout and capability mask
          • Wrote and read back actuator state in scratch memory
          • Executed a timed reverse/forward motor sequence
          • Completed a read-verify-idle sensing cycle

        At no point did the AI have direct hardware access.
        Every command passed through the KAI verifier and was
        bounded by the MMU-enforced scratch region at 0x40200000.
    """).strip(), C.DIM))
    print()

def main():
    global USE_COLOR

    parser = argparse.ArgumentParser(description="KAI OS AI Demo")
    parser.add_argument("--socket", default="/tmp/kai.sock")
    parser.add_argument("--delay", type=float, default=1.4)
    parser.add_argument("--no-color", action="store_true")
    args = parser.parse_args()

    if args.no_color:
        USE_COLOR = False

    print(colorize("\n  KAI OS — AI Agent Demo", C.BOLD))
    print(colorize(f"  Socket : {args.socket}", C.DIM))
    print(colorize(f"  Delay  : {args.delay}s per step\n", C.DIM))
    print(colorize(
        "  Start the kernel first in another terminal:\n"
        "  qemu-system-aarch64 -M virt -cpu max -nographic \\\n"
        "    -kernel build/kernel.elf \\\n"
        "    -serial unix:/tmp/kai.sock,server,nowait \\\n"
        "    -monitor none 2>/dev/null\n",
        C.DIM
    ))

    try:
        sock = connect(args.socket)
        print(colorize("\n  Connected.\n", C.AI))
        run_demo(sock, args.delay)
        sock.close()
    except RuntimeError as e:
        print_error(str(e))
        sys.exit(1)
    except KeyboardInterrupt:
        print(colorize("\n\n  Demo interrupted.", C.DIM))
        sys.exit(0)

if __name__ == "__main__":
    main()