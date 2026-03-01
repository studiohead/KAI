/*
 * include/kernel/uart.h — PL011 UART public interface
 *
 * Only the API surface is exposed here.  Register addresses and bit-field
 * constants live in uart.c so that nothing outside the driver can reach
 * them directly.
 */

#ifndef KERNEL_UART_H
#define KERNEL_UART_H

#include <stdint.h>

/*
 * uart_init() — must be called before any other uart_* function.
 * Safe to call multiple times (idempotent).
 */
void uart_init(void);

/* Blocking single-character output / input */
void uart_putc(char c);
char uart_getc(void);

/* Convenience wrappers */
void uart_puts(const char *s);
void uart_hex64(uint64_t value);
void uart_dec(uint32_t value);

#endif /* KERNEL_UART_H */
