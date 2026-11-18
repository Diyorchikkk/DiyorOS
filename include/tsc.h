/*
 * tsc.h — protsessorning takt hisoblagichi (Time Stamp Counter), v0.11.
 *
 * Taymer (PIT) 100 Hz: eng kichik o'lchov birligi 10 ms. Kontekst almashtirish
 * yoki syscall bir necha mikrosekund davom etadi — taymer ularni ko'rmaydi.
 * TSC — protsessor ichidagi 64 bitli hisoblagich: har taktda +1. "rdtsc" buyrug'i
 * uni o'qiydi. Yuklanishda uning chastotasini PIT bilan solishtirib topamiz
 * (kalibrlash): 50 ms ichida necha takt o'tdi -> kHz.
 *
 * Kernel libgcc siz yig'iladi: 64 bitli bo'lish (__udivdi3) yo'q. Shuning uchun
 * udiv64 — ikkita 32 bitli "divl" buyrug'i bilan (maktabdagi ustunli bo'lish kabi).
 */
#ifndef TSC_H
#define TSC_H

#include <stdint.h>
#include <stdbool.h>

static inline uint64_t rdtsc(void)
{
    uint32_t lo, hi;
    __asm__ volatile ("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

/* 64 bitli sonni 32 bitliga bo'lish (d > 0) */
static inline uint64_t udiv64(uint64_t n, uint32_t d)
{
    uint32_t hi = (uint32_t)(n >> 32), lo = (uint32_t)n;
    uint32_t qhi = hi / d, r = hi % d, qlo;
    __asm__ ("divl %4" : "=a"(qlo), "=d"(r) : "a"(lo), "d"(r), "rm"(d));
    return ((uint64_t)qhi << 32) | qlo;
}

void     tsc_calibrate(void);          /* uzilishlar yoqilgandan keyin, bir marta */
bool     tsc_available(void);
uint32_t tsc_khz(void);                /* protsessor chastotasi, kHz (0 — TSC yo'q) */
uint32_t tsc_to_us(uint64_t cycles);   /* taktlar -> mikrosekund */
uint32_t tsc_to_ns(uint64_t cycles);   /* taktlar -> nanosekund (4 soniyagacha) */
uint64_t tsc_now_us(void);             /* yuklanishdan beri, mikrosekund */

#endif
