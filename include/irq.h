/*
 * irq.h — kritik seksiyalar: uzilishlarni vaqtincha o'chirish.
 *
 * Bitta protsessorli tizimda vazifa faqat uzilish (taymer) orqali
 * almashadi. Demak uzilishlar o'chiq bo'lsa, kod "bo'linmas" bajariladi:
 * hech kim o'rtada xalaqit bermaydi. Umumiy ma'lumotlar (heap, ekran,
 * vazifalar jadvali) shu usul bilan himoyalanadi.
 *
 *   uint32_t f = irq_save();   // o'chirish, avvalgi holatni eslab qolish
 *   ... kritik seksiya ...
 *   irq_restore(f);            // avvalgi holatni qaytarish
 */
#ifndef IRQ_H
#define IRQ_H

#include <stdint.h>

static inline uint32_t irq_save(void)
{
    uint32_t flags;
    __asm__ volatile ("pushf; pop %0; cli" : "=r"(flags) : : "memory");
    return flags;
}

static inline void irq_restore(uint32_t flags)
{
    if (flags & 0x200)                 /* IF biti: avval yoqilgan bo'lsa */
        __asm__ volatile ("sti" : : : "memory");
}

#endif
