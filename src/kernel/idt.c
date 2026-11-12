/*
 * idt.c — Interrupt Descriptor Table (IDT) va PIC.
 *
 * Uzilish (interrupt) — protsessorni joriy ishini to'xtatib, boshqa
 * kodni bajarishga majbur qiluvchi signal. Ikki turi bor:
 *   1) Exception (0-31): protsessorning o'zi xato topganda
 *      (masalan 0 ga bo'lish, noto'g'ri xotiraga murojaat).
 *   2) IRQ (32-47): tashqi qurilmalardan (taymer, klaviatura...).
 *
 * IDT — har bir uzilish raqami uchun qaysi funksiya chaqirilishini
 * ko'rsatuvchi jadval (256 ta yozuv).
 *
 * PIC (8259) — qurilmalar uzilishlarini protsessorga yetkazuvchi chip.
 * Standart holatda u IRQ larni 8-15 raqamlarga yuboradi va ular
 * exceptionlar bilan to'qnashadi. Shuning uchun uni 32-47 ga "remap"
 * qilamiz.
 */
#include <stdint.h>
#include "cpu.h"
#include "io.h"
#include "vga.h"
#include "kprintf.h"
#include "task.h"
#include "process.h"
#include "klog.h"
#include <stdbool.h>

#define PIC1_CMD  0x20
#define PIC1_DATA 0x21
#define PIC2_CMD  0xA0
#define PIC2_DATA 0xA1
#define PIC_EOI   0x20   /* "End Of Interrupt" — uzilishga ishlov berildi */

struct idt_entry {
    uint16_t base_low;
    uint16_t selector;   /* qaysi kod segmenti (0x08 — kernel kodi) */
    uint8_t  zero;
    uint8_t  flags;      /* 0x8E: mavjud, ring 0, 32-bit interrupt gate */
    uint16_t base_high;
} __attribute__((packed));

struct idt_ptr {
    uint16_t limit;
    uint32_t base;
} __attribute__((packed));

static struct idt_entry idt[256];
static struct idt_ptr   idt_pointer;
static irq_handler_t    irq_handlers[16];

extern uint32_t isr_stub_table[48];   /* interrupts.s da */
extern void idt_load(uint32_t);
extern void isr128(void);          /* syscall stub */
extern void isr129(void);          /* task_yield stub */

registers_t *interrupt_handler(registers_t *r);   /* interrupts.s chaqiradi */

static const char *exception_names[32] = {
    "Division By Zero (0 ga bo'lish)", "Debug", "Non Maskable Interrupt",
    "Breakpoint", "Overflow", "Bound Range Exceeded", "Invalid Opcode",
    "Device Not Available", "Double Fault", "Coprocessor Segment Overrun",
    "Invalid TSS", "Segment Not Present", "Stack-Segment Fault",
    "General Protection Fault", "Page Fault", "Reserved",
    "x87 Floating-Point", "Alignment Check", "Machine Check",
    "SIMD Floating-Point", "Virtualization", "Control Protection",
    "Reserved", "Reserved", "Reserved", "Reserved", "Reserved", "Reserved",
    "Hypervisor Injection", "VMM Communication", "Security", "Reserved"
};

static void idt_set(int i, uint32_t base, uint16_t sel, uint8_t flags)
{
    idt[i].base_low  = base & 0xFFFF;
    idt[i].base_high = (base >> 16) & 0xFFFF;
    idt[i].selector  = sel;
    idt[i].zero      = 0;
    idt[i].flags     = flags;
}

static void pic_remap(void)
{
    outb(PIC1_CMD, 0x11);  io_wait();   /* ICW1: ishga tushirish */
    outb(PIC2_CMD, 0x11);  io_wait();
    outb(PIC1_DATA, 0x20); io_wait();   /* ICW2: master IRQ 0-7  -> 32-39 */
    outb(PIC2_DATA, 0x28); io_wait();   /* ICW2: slave  IRQ 8-15 -> 40-47 */
    outb(PIC1_DATA, 0x04); io_wait();   /* ICW3: slave IRQ2 ga ulangan */
    outb(PIC2_DATA, 0x02); io_wait();
    outb(PIC1_DATA, 0x01); io_wait();   /* ICW4: 8086 rejimi */
    outb(PIC2_DATA, 0x01); io_wait();

    /* Yoqilgan IRQ lar: 0 (taymer), 1 (klaviatura), 2 (kaskad), 12 (sichqoncha) */
    outb(PIC1_DATA, 0xF8);
    outb(PIC2_DATA, 0xEF);
}

void idt_init(void)
{
    idt_pointer.limit = sizeof(idt) - 1;
    idt_pointer.base  = (uint32_t)&idt;

    for (int i = 0; i < 48; i++)
        idt_set(i, isr_stub_table[i], 0x08, 0x8E);
    /* #8 Double Fault: task gate (0x85) — protsessor alohida TSS va toza stekka o'tadi */
    idt_set(8, 0, DF_TSS_SEL, 0x85);
    idt_set(YIELD_VECTOR, (uint32_t)isr129, 0x08, 0x8E);
    /* 0xEE: DPL = 3 — ring 3 dagi dastur ham "int 0x80" ni chaqira oladi.
       Boshqa barcha uzilishlarni (0x8E, DPL = 0) dastur chaqirsa — General Protection Fault */
    idt_set(SYSCALL_VECTOR, (uint32_t)isr128, 0x08, 0xEE);

    pic_remap();
    idt_load((uint32_t)&idt_pointer);
}

void irq_register(int irq, irq_handler_t handler)
{
    if (irq >= 0 && irq < 16)
        irq_handlers[irq] = handler;
}

/* Har bir IRQ necha marta kelgani (/proc/interrupts) */
static uint32_t irq_counts[16];
static uint32_t spurious_count;

uint32_t irq_count(int irq)
{
    return (irq >= 0 && irq < 16) ? irq_counts[irq] : 0;
}

uint32_t irq_spurious_count(void)
{
    return spurious_count;
}

/*
 * "Soxta" uzilish (spurious IRQ): elektr shovqini sababli PIC ba'zan IRQ7
 * (yoki IRQ15) ni yuboradi, lekin aslida hech bir qurilma so'ramagan.
 * Farqlash uchun PIC ning ISR registrini o'qiymiz: haqiqiy uzilishda mos
 * bit yoqilgan bo'ladi. Soxtasiga EOI yuborilmaydi (IRQ15 da faqat masterga).
 */
static bool irq_is_spurious(int irq)
{
    if (irq == 7) {
        outb(PIC1_CMD, 0x0B);                  /* "ISR registrini o'qiyman" */
        return !(inb(PIC1_CMD) & 0x80);
    }
    if (irq == 15) {
        outb(PIC2_CMD, 0x0B);
        if (!(inb(PIC2_CMD) & 0x80)) {
            outb(PIC1_CMD, PIC_EOI);           /* master kaskad (IRQ2) ni haqiqiy deb biladi */
            return true;
        }
    }
    return false;
}

/*
 * interrupts.s dagi isr_common shu funksiyani chaqiradi.
 * Qaytariladigan qiymat — qaysi stekdagi registrlarni tiklash kerak:
 * odatda o'sha r, lekin scheduler boshqa vazifani tanlasa — uning registrlari.
 */
registers_t *interrupt_handler(registers_t *r)
{
    if (r->int_no < 32) {
        if (r->int_no == 7 && fpu_handle_nm(r))
            return r;                          /* FPU holati almashtirildi — dastur davom etadi */
        if ((r->cs & 3) == 3) {                /* xato dasturda (ring 3) — faqat u o'ladi */
            process_fault(r, exception_names[r->int_no]);
            return sched_maybe_switch(r);
        }
        panic_exception(r, exception_names[r->int_no]);   /* kernelning o'zida — tizim to'xtaydi */
    }

    if (r->int_no == SYSCALL_VECTOR) {
        syscall_handler(r);
        return sched_maybe_switch(r);
    }

    if (r->int_no == YIELD_VECTOR)
        return sched_yield_handler(r);

    int irq = (int)r->int_no - 32;
    if (irq >= 0 && irq < 16) {
        if (irq_is_spurious(irq)) {
            spurious_count++;
            return r;
        }
        irq_counts[irq]++;
        if (irq_handlers[irq])
            irq_handlers[irq](r);

        /* PIC ga "tugatdim" signalini yuboramiz, aks holda keyingi IRQ kelmaydi.
           Bu vazifa almashtirishdan OLDIN bo'lishi shart. */
        if (irq >= 8)
            outb(PIC2_CMD, PIC_EOI);
        outb(PIC1_CMD, PIC_EOI);
    }

    return sched_maybe_switch(r);
}
