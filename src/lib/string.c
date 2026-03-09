/*
 * src/lib/string.c — Freestanding string utilities
 *
 * These are prefixed with k_ to avoid any symbol-level conflict with a
 * future hosted libc if the build system ever links one for tooling.
 */

#include <kernel/string.h>
#include <stdint.h>

/* ======================================================================
 * k_strcmp — lexicographic comparison; returns 0 if equal.
 * ====================================================================== */
int k_strcmp(const char *s1, const char *s2)
{
    while (*s1 && (*s1 == *s2)) {
        s1++;
        s2++;
    }
    return (int)(unsigned char)*s1 - (int)(unsigned char)*s2;
}

/* ======================================================================
 * k_strncmp — lexicographic comparison of at most n bytes.
 * Returns 0 if equal up to n bytes, <0 if s1 < s2, >0 if s1 > s2.
 * ====================================================================== */
int k_strncmp(const char *s1, const char *s2, size_t n)
{
    if (n == 0)
        return 0;

    while (n-- > 0) {
        if (*s1 != *s2)
            return (int)(unsigned char)*s1 - (int)(unsigned char)*s2;
        if (*s1 == '\0')
            return 0;
        s1++;
        s2++;
    }
    return 0;
}

/* ======================================================================
 * k_strlen — number of bytes before NUL terminator.
 * ====================================================================== */
size_t k_strlen(const char *s)
{
    const char *p = s;
    while (*p)
        p++;
    return (size_t)(p - s);
}

/* ======================================================================
 * k_memset — fill n bytes at dst with byte value c.
 * ====================================================================== */
void *k_memset(void *dst, int c, size_t n)
{
    unsigned char *p = (unsigned char *)dst;
    while (n--)
        *p++ = (unsigned char)c;
    return dst;
}

/* ======================================================================
 * k_strcpy — copy src to dst including terminating NUL.
 * Assumes dst is large enough (we always use CMD_BUF_SIZE).
 * ====================================================================== */
char *k_strcpy(char *dst, const char *src)
{
    char *d = dst;
    while ((*d++ = *src++) != '\0')
        ;
    return dst;
}

/* ======================================================================
 * k_memcpy — copy n bytes from src to dst.
 * Basic implementation for bare-metal AArch64.
 * ====================================================================== */
void *k_memcpy(void *dst, const void *src, size_t n) {
    unsigned char *d = (unsigned char *)dst;
    const unsigned char *s = (const unsigned char *)src;
    for (size_t i = 0; i < n; i++) {
        d[i] = s[i];
    }
    return dst;
}

/* Standard library aliases for the compiler */
void *memcpy(void *dest, const void *src, size_t n) __attribute__((alias("k_memcpy")));
void *memset(void *s, int c, size_t n) __attribute__((alias("k_memset")));