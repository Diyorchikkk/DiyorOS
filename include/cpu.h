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
    uint32_t useresp, ss;     /* faqat ring 3 dan uzilish kelganda (v0.5) */
} registers_t;

typedef void (*irq_handler_t)(registers_t *regs);

void gdt_init(void);
void idt_init(void);
void irq_register(int irq, irq_handler_t handler);
uint32_t irq_count(int irq);          /* shu IRQ necha marta kelgan (v0.6.1) */
uint32_t irq_spurious_count(void);

#define YIELD_VECTOR   0x81   /* task_yield() uzilishi */
#define SYSCALL_VECTOR 0x80   /* dasturlar kernelga murojaati: int 0x80 (v0.5) */

/* Segment selektorlari (GDT dagi indeks * 8 + ruxsat darajasi) */
#define KERNEL_CS 0x08
#define KERNEL_DS 0x10
#define USER_CS   (0x18 | 3)
#define USER_DS   (0x20 | 3)
#define TSS_SEL   0x28
#define DF_TSS_SEL 0x30  /* Double Fault uchun alohida TSS (task gate) */

/* TSS: dastur (ring 3) dan uzilish kelganda protsessor qaysi kernel stekiga o'tishi */
void tss_set_kernel_stack(uint32_t esp0);
void tss_set_df_cr3(uint32_t cr3);
void tss_saved_state(uint32_t *eip, uint32_t *esp, uint32_t *ebp);

#endif
