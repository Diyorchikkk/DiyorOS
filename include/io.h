/*
 * io.h — qurilmalar bilan "port" orqali ishlash.
 * x86 da klaviatura, taymer, PIC kabi qurilmalarga maxsus IN/OUT
 * buyruqlari orqali murojaat qilinadi. C da bular yo'q, shuning uchun
 * inline assembly ishlatamiz.
 */
#ifndef IO_H
#define IO_H

#include <stdint.h>

/* Portga 1 bayt yozish */
static inline void outb(uint16_t port, uint8_t val)
{
    __asm__ volatile ("outb %0, %1" : : "a"(val), "Nd"(port));
}

/* Portga 2 bayt yozish */
static inline void outw(uint16_t port, uint16_t val)
{
    __asm__ volatile ("outw %0, %1" : : "a"(val), "Nd"(port));
}

/* Portdan 1 bayt o'qish */
static inline uint8_t inb(uint16_t port)
{
    uint8_t ret;
    __asm__ volatile ("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

/* Eski qurilmalar ulgurishi uchun qisqa kutish */
static inline void io_wait(void)
{
    outb(0x80, 0);
}

#endif
