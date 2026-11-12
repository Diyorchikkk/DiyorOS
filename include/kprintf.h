/* kprintf.h — formatlab chiqarish (printf ning kernel varianti) */
#ifndef KPRINTF_H
#define KPRINTF_H

#include <stdarg.h>
#include <stdint.h>

/* Qo'llab-quvvatlanadi: %d %u %x %s %c %% hamda kenglik: %02d, %8x, %-10s (chapga) */
void kprintf(const char *fmt, ...);

/* Buferga yozish (snprintf). Qaytaradi: to'liq natija uzunligi (sig'masa ham) */
int  ksnprintf(char *buf, uint32_t size, const char *fmt, ...);
int  kvsnprintf(char *buf, uint32_t size, const char *fmt, va_list ap);

#endif
