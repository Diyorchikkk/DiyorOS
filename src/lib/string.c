/*
 * string.c — xotira va satrlar bilan ishlash funksiyalari.
 * Kernelda standart C kutubxonasi (libc) yo'q: hamma narsani o'zimiz yozamiz.
 * Kompilyator ham ba'zan memset/memcpy ni o'zi chaqiradi, shuning uchun ular shart.
 */
#include "string.h"
#include <stdint.h>

void *memset(void *dst, int val, size_t n)
{
    uint8_t *d = dst;
    while (n--)
        *d++ = (uint8_t)val;
    return dst;
}

void *memcpy(void *dst, const void *src, size_t n)
{
    uint8_t *d = dst;
    const uint8_t *s = src;
    while (n--)
        *d++ = *s++;
    return dst;
}

/* memcpy dan farqi: manba va maqsad ustma-ust tushsa ham to'g'ri ishlaydi */
void *memmove(void *dst, const void *src, size_t n)
{
    uint8_t *d = dst;
    const uint8_t *s = src;
    if (d < s) {
        while (n--)
            *d++ = *s++;
    } else {
        d += n;
        s += n;
        while (n--)
            *--d = *--s;
    }
    return dst;
}

int memcmp(const void *a, const void *b, size_t n)
{
    const uint8_t *x = a, *y = b;
    for (; n; n--, x++, y++)
        if (*x != *y)
            return *x - *y;
    return 0;
}

size_t strlen(const char *s)
{
    size_t n = 0;
    while (s[n])
        n++;
    return n;
}

int strcmp(const char *a, const char *b)
{
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return (uint8_t)*a - (uint8_t)*b;
}

int strncmp(const char *a, const char *b, size_t n)
{
    for (; n; n--, a++, b++) {
        if (*a != *b)
            return (uint8_t)*a - (uint8_t)*b;
        if (!*a)
            return 0;
    }
    return 0;
}

char *strcpy(char *dst, const char *src)
{
    char *d = dst;
    while ((*d++ = *src++))
        ;
    return dst;
}

char *strncpy(char *dst, const char *src, size_t n)
{
    size_t i = 0;
    for (; i < n && src[i]; i++)
        dst[i] = src[i];
    for (; i < n; i++)
        dst[i] = 0;
    return dst;
}
