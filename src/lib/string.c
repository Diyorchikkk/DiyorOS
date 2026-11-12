/*
 * string.c — xotira va satrlar bilan ishlash funksiyalari.
 * Kernelda standart C kutubxonasi (libc) yo'q: hamma narsani o'zimiz yozamiz.
 * Kompilyator ham ba'zan memset/memcpy ni o'zi chaqiradi, shuning uchun ular shart.
 *
 * v0.6.1: memset/memcpy/memmove protsessorning "rep stos/movs" buyruqlari
 * bilan ishlaydi — baytma-bayt tsikldan bir necha marta tez (ekranni surish,
 * sahifalarni nollash, fayllarni nusxalash shularga tayanadi).
 */
#include "string.h"
#include <stdint.h>

void *memset(void *dst, int val, size_t n)
{
    void *d = dst;
    uint32_t fill = (uint8_t)val * 0x01010101u;      /* baytni 4 marta takrorlash */
    size_t words = n / 4, tail = n % 4;
    __asm__ volatile ("cld; rep stosl" : "+D"(d), "+c"(words) : "a"(fill) : "memory", "cc");
    __asm__ volatile ("rep stosb" : "+D"(d), "+c"(tail) : "a"(fill) : "memory", "cc");
    return dst;
}

void *memcpy(void *dst, const void *src, size_t n)
{
    void *d = dst;
    const void *s = src;
    size_t words = n / 4, tail = n % 4;
    __asm__ volatile ("cld; rep movsl" : "+D"(d), "+S"(s), "+c"(words) : : "memory", "cc");
    __asm__ volatile ("rep movsb" : "+D"(d), "+S"(s), "+c"(tail) : : "memory", "cc");
    return dst;
}

/* memcpy dan farqi: manba va maqsad ustma-ust tushsa ham to'g'ri ishlaydi */
void *memmove(void *dst, const void *src, size_t n)
{
    uint8_t *d = dst;
    const uint8_t *s = src;
    if (d == s || n == 0)
        return dst;
    if (d < s || d >= s + n)
        return memcpy(dst, src, n);                  /* oldinga nusxalash xavfsiz */

    /* Ustma-ust va maqsad keyinroq: oxiridan boshlab, teskari yo'nalishda (std) */
    d += n - 1;
    s += n - 1;
    __asm__ volatile ("std; rep movsb; cld" : "+D"(d), "+S"(s), "+c"(n) : : "memory", "cc");
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

/* n baytgacha nusxalaydi; qolgan joy nol bilan to'ldiriladi (standart strncpy kabi:
   manba n dan uzun bo'lsa, oxirida nol BO'LMAYDI — chaqiruvchi o'zi qo'yadi) */
char *strncpy(char *dst, const char *src, size_t n)
{
    size_t i = 0;
    for (; i < n && src[i]; i++)
        dst[i] = src[i];
    for (; i < n; i++)
        dst[i] = 0;
    return dst;
}

/* Xavfsiz nusxalash: doim nol bilan tugaydi, size dan oshmaydi. Manba uzunligini qaytaradi */
size_t strlcpy(char *dst, const char *src, size_t size)
{
    size_t len = strlen(src);
    if (size) {
        size_t n = len < size - 1 ? len : size - 1;
        memcpy(dst, src, n);
        dst[n] = 0;
    }
    return len;
}
