/*
 * include/kernel/string.h — Minimal freestanding string utilities
 *
 * Only what the kernel actually uses is declared here.  No dependency on
 * a hosted libc <string.h>.
 */

#ifndef KERNEL_STRING_H
#define KERNEL_STRING_H

#include <stddef.h>

int    k_strcmp(const char *s1, const char *s2);
int    k_strncmp(const char *s1, const char *s2, size_t n);
size_t k_strlen(const char *s);
void  *k_memset(void *dst, int c, size_t n);
char  *k_strcpy(char *dst, const char *src);

#endif /* KERNEL_STRING_H */