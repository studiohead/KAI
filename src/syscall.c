/*
 * src/syscall.c — KAI OS safe syscall interface
 *
 * Provides capability-gated access to UART output.
 *
 * Memory syscalls (sys_mem_read, sys_mem_info) live in memory.c
 * since they directly access linker symbols and the region table.
 *
 * Address whitelisting is NOT applied to sys_uart_write — buf is
 * always kernel memory (.rodata, stack, .data), all of which are
 * trusted. Whitelisting is enforced by sys_mem_read for untrusted
 * sandbox reads only.
 */

#include <kernel/syscall.h>
#include <kernel/uart.h>
#include <stddef.h>
#include <stdint.h>

/* ============================================================================
 * sys_uart_write
 *
 * Writes 'len' bytes from 'buf' to UART.
 * Requires CAP_MMIO.
 *
 * Allowed characters:
 *   - Printable ASCII (0x20-0x7E)
 *   - \n, \r, \t  (standard whitespace)
 *   - \b          (backspace, used in "\b \b" erase sequence)
 *
 * Returns number of bytes written, or -1 on failure.
 * ========================================================================== */
int sys_uart_write(const char *buf, size_t len, uint32_t caller_caps)
{
    if (!(caller_caps & CAP_MMIO))
        return -1;

    if (!buf || len == 0)
        return -1;

    const char *p = buf;
    const char * const end = buf + len;

    while (p < end)
    {
        const unsigned char c = (unsigned char)*p;

        if (c == 0x7FU)
            return -1;

        if (c < 0x20U && c != '\n' && c != '\r' && c != '\t' && c != '\b')
            return -1;

        uart_putc((char)c);

        ++p;
    }

    return (int)len;
}

/* ============================================================================
 * sys_uart_hex64
 *
 * Writes a 64-bit value as exactly 16 uppercase hex digits to UART.
 * Capability enforced by the sys_uart_write call internally.
 *
 * Returns 16 on success, -1 on failure.
 * ========================================================================== */
int sys_uart_hex64(uint64_t val, uint32_t caller_caps)
{
    char buf[16];

    for (int i = 0; i < 16; i++) {
        uint8_t nibble = (uint8_t)((val >> (60 - i * 4)) & 0xFU);
        buf[i] = (nibble < 10U) ? (char)('0' + nibble)
                                : (char)('A' + nibble - 10U);
    }

    return sys_uart_write(buf, 16, caller_caps);
}