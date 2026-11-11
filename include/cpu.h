/* cpu.h — GDT, IDT va uzilishlar bilan bog'liq e'lonlar */
#ifndef CPU_H
#define CPU_H

#include <stdint.h>

/* Uzilish paytida stekda saqlangan registrlar (interrupts.s dagi tartibda) */
typedef struct {
    uint32_t gs, fs, es, ds;
    uint32_t edi, esi, ebp, esp_dummy, ebx, edx, ecx, eax;  /* pusha */
    uint32_t int_no, err_code;                              /* stub qo'ygan */
    uint32_t eip, cs, eflags;                               /* protsessor qo'ygan */
} registers_t;

typedef void (*irq_handler_t)(registers_t *regs);

void gdt_init(void);
void idt_init(void);
void irq_register(int irq, irq_handler_t handler);

#endif
