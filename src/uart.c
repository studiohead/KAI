/*
 * src/uart.c — PL011 UART driver for QEMU virt board
 *
 * Base address 0x09000000 matches the QEMU -M virt memory map.
 * Register offsets follow the ARM PrimeCell UART (PL011) TRM.
 *
 * All hardware access goes through the mmio_* helpers; no raw casts here.
 */

#include <kernel/uart.h>
#include <kernel/mmio.h>
#include <stdint.h>

/* ---- PL011 register offsets (all 32-bit) ----------------------------- */
#define UART_BASE       ((uintptr_t)0x09000000UL)

#define UART_DR         (UART_BASE + 0x000U)  /* Data register            */
#define UART_FR         (UART_BASE + 0x018U)  /* Flag register            */
#define UART_IBRD       (UART_BASE + 0x024U)  /* Integer baud rate        */
#define UART_FBRD       (UART_BASE + 0x028U)  /* Fractional baud rate     */
#define UART_LCR_H      (UART_BASE + 0x02CU)  /* Line control             */
#define UART_CR         (UART_BASE + 0x030U)  /* Control register         */

/* ---- Flag register bits ---------------------------------------------- */
#define FR_TXFF         (1U << 5)   /* TX FIFO full  */
#define FR_RXFE         (1U << 4)   /* RX FIFO empty */

/* ---- Control / line-control bits ------------------------------------- */
#define CR_UARTEN       (1U << 0)   /* UART enable       */
#define CR_TXE          (1U << 8)   /* TX enable         */
#define CR_RXE          (1U << 9)   /* RX enable         */
#define LCR_FEN         (1U << 4)   /* FIFO enable       */
#define LCR_WLEN_8      (3U << 5)   /* 8-bit word length */

/* ======================================================================
 * uart_init
 *
 * Configure the PL011 for 115200 8N1 with FIFO enabled.
 * QEMU does not require this — the UART works at reset — but real
 * hardware does, and explicit initialisation is always safer.
 * ====================================================================== */
void uart_init(void)
{
    /* Disable UART before changing config */
    mmio_write32(UART_CR, 0U);

    /*
     * Baud rate divisor for 115200 with a 24 MHz UARTCLK:
     *   BRD = 24000000 / (16 * 115200) = 13.020833...
     *   IBRD = 13, FBRD = round(0.020833 * 64) = 1
     */
    mmio_write32(UART_IBRD, 13U);
    mmio_write32(UART_FBRD,  1U);

    /* 8N1, FIFO enabled */
    mmio_write32(UART_LCR_H, LCR_WLEN_8 | LCR_FEN);

    /* Enable UART, TX, RX */
    mmio_write32(UART_CR, CR_UARTEN | CR_TXE | CR_RXE);
}

/* ======================================================================
 * uart_putc — block until TX FIFO has room, then write one byte.
 * ====================================================================== */
void uart_putc(char c)
{
    while (mmio_read32(UART_FR) & FR_TXFF)
        ; /* spin */
    mmio_write32(UART_DR, (uint32_t)(unsigned char)c);
}

/* ======================================================================
 * uart_getc — block until RX FIFO has data, then read one byte.
 * ====================================================================== */
char uart_getc(void)
{
    while (mmio_read32(UART_FR) & FR_RXFE)
        ; /* spin */
    return (char)(mmio_read32(UART_DR) & 0xFFU);
}

/* ======================================================================
 * uart_puts — write a NUL-terminated string.
 * Translates bare '\n' to '\r\n' so terminals render correctly.
 * ====================================================================== */
void uart_puts(const char *s)
{
    while (*s) {
        if (*s == '\n')
            uart_putc('\r');
        uart_putc(*s++);
    }
}

/* ======================================================================
 * uart_hex64 — print a 64-bit value as "0x<16 hex digits>".
 * ====================================================================== */
void uart_hex64(uint64_t value)
{
    uart_puts("0x");
    for (int shift = 60; shift >= 0; shift -= 4) {
        uint8_t nibble = (uint8_t)((value >> shift) & 0xFU);
        uart_putc(nibble < 10u ? (char)('0' + nibble)
                               : (char)('A' + nibble - 10u));
    }
}

/* ======================================================================
 * uart_dec — print an unsigned 32-bit value in decimal.
 * ====================================================================== */
void uart_dec(uint32_t value)
{
    if (value == 0U) {
        uart_putc('0');
        return;
    }

    char buf[10]; /* 2^32 - 1 = 4294967295, 10 digits */
    int  i = 0;

    while (value > 0U) {
        buf[i++] = (char)('0' + (value % 10U));
        value /= 10U;
    }

    /* Digits are in reverse order */
    while (i > 0)
        uart_putc(buf[--i]);
}
