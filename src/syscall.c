/*
 * src/syscall.c — KAI OS safe syscall interface
 *
 * Provides capability-gated access to UART output and IRQ management.
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
#include <kernel/irq.h>
#include <stddef.h>
#include <stdint.h>

/* ============================================================================
 * sys_uart_write
 *
 * Writes 'len' bytes from 'buf' to UART.
 * Requires CAP_MMIO.
 *
 * Allowed characters:
 * - Printable ASCII (0x20-0x7E)
 * - \n, \r, \t  (standard whitespace)
 * - \b          (backspace, used in "\b \b" erase sequence)
 * - \033 [      (Escape sequence start for terminal control)
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

        /* Basic filtering for safety, but allow escape sequences for terminal control */
        if (c == 0x7FU)
            return -1;

        /* Allow printable ASCII, standard whitespace, and the ESC character (0x1B) 
         * ESC is required for the 'clear' command to function via sys_uart_write. */
        if (c < 0x20U && c != '\n' && c != '\r' && c != '\t' && c != '\b' && c != 0x1BU)
            return -1;

        uart_putc((char)c);

        ++p;
    }

    return (int)len;
}

/* ============================================================================
 * sys_uart_hex64
 *
 * Writes a 64-bit value as exactly 16 uppercase hex digits (with 0x prefix) 
 * to the UART. 
 *
 * Total output: 18 characters (0x + 16 hex digits).
 * * Capability check: Enforced by the sys_uart_write call internally.
 * Returns: 18 on success, -1 on failure.
 * ========================================================================== */
int sys_uart_hex64(uint64_t val, uint32_t caller_caps)
{
    /* Buffer for '0x' + 16 hex digits. No null terminator needed for sys_uart_write */
    char buf[18];
    
    buf[0] = '0';
    buf[1] = 'x';

    /* * Extract nibbles (4 bits) starting from the most significant bit.
     * We shift right by (60, 56, 52, ..., 0) bits and mask the bottom 4 bits.
     */
    for (int i = 0; i < 16; i++) {
        uint8_t nibble = (uint8_t)((val >> (60 - (i * 4))) & 0x0FULL);
        
        if (nibble < 10U) {
            buf[i + 2] = (char)('0' + nibble);
        } else {
            /* 10-15 map to A-F */
            buf[i + 2] = (char)('A' + (nibble - 10U));
        }
    }

    /* * Hand the buffer to the safe write syscall. 
     * sys_uart_write handles the CAP_MMIO check and character validation.
     */
    return sys_uart_write(buf, 18, caller_caps);
}

/* ============================================================================
 * sys_uart_puts
 *
 * Writes a null-terminated string to UART using the safe syscall layer.
 * Capability enforcement is handled by sys_uart_write().
 * ========================================================================== */
int sys_uart_puts(const char *s, uint32_t caps)
{
    if (!s) return -1;

    /* Walk pointer forward until null terminator */
    const char *p = s;
    while (*p)
        ++p;

    /* Length = end pointer - start pointer */
    return sys_uart_write(s, (size_t)(p - s), caps);
}

/* ============================================================================
 * sys_uart_putc
 *
 * Writes a single character to UART using the safe syscall layer.
 * ========================================================================== */
int sys_uart_putc(char c, uint32_t caps)
{
    return sys_uart_write(&c, 1, caps);
}

/* ============================================================================
 * sys_irq_control
 *
 * Allows a sandboxed AI to enable or disable specific hardware IRQs.
 * Requires CAP_IRQ (a new capability bit for hardware control).
 *
 * This allows a 'manager' pipeline to switch other 'reflexes' on or off.
 *
 * Returns 0 on success, -1 on failure.
 * ========================================================================== */
int sys_irq_control(uint32_t irq_num, bool enable, uint32_t caller_caps)
{
    /* Note: You may need to define CAP_IRQ in your sandbox.h if not present */
    if (!(caller_caps & CAP_MMIO)) 
        return -1;

    if (enable) {
        irq_enable(irq_num);
    } else {
        irq_disable(irq_num);
    }

    return 0;
}