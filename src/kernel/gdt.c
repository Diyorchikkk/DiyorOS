/*
 * gdt.c — Global Descriptor Table (GDT).
 *
 * 32-bit protected mode da protsessor xotiraga "segmentlar" orqali
 * murojaat qiladi. GDT — bu segmentlar jadvali: har biri qaysi
 * manzildan boshlanishi, hajmi va ruxsatlari (kernel yoki foydalanuvchi,
 * kod yoki ma'lumot) yozilgan.
 *
 * Biz "flat" model ishlatamiz: barcha segmentlar 0 dan 4 GB gacha.
 * Ya'ni xotira himoyasi keyinchalik paging orqali qilinadi, GDT esa
 * asosan ruxsat darajasini (ring 0 — kernel, ring 3 — dastur) belgilaydi.
 */
#include <stdint.h>
#include "cpu.h"

struct gdt_entry {
    uint16_t limit_low;
    uint16_t base_low;
    uint8_t  base_middle;
    uint8_t  access;       /* mavjudlik, ruxsat darajasi, kod/ma'lumot */
    uint8_t  granularity;  /* limitning yuqori 4 biti + flaglar */
    uint8_t  base_high;
} __attribute__((packed));

struct gdt_ptr {
    uint16_t limit;
    uint32_t base;
} __attribute__((packed));

static struct gdt_entry gdt[5];
static struct gdt_ptr   gdt_pointer;

extern void gdt_flush(uint32_t);   /* interrupts.s da */

static void gdt_set(int i, uint32_t base, uint32_t limit, uint8_t access, uint8_t gran)
{
    gdt[i].base_low    = base & 0xFFFF;
    gdt[i].base_middle = (base >> 16) & 0xFF;
    gdt[i].base_high   = (base >> 24) & 0xFF;
    gdt[i].limit_low   = limit & 0xFFFF;
    gdt[i].granularity = ((limit >> 16) & 0x0F) | (gran & 0xF0);
    gdt[i].access      = access;
}

void gdt_init(void)
{
    gdt_pointer.limit = sizeof(gdt) - 1;
    gdt_pointer.base  = (uint32_t)&gdt;

    gdt_set(0, 0, 0, 0, 0);                  /* 0: majburiy bo'sh yozuv     */
    gdt_set(1, 0, 0xFFFFFFFF, 0x9A, 0xCF);   /* 1: kernel kodi    (ring 0)  */
    gdt_set(2, 0, 0xFFFFFFFF, 0x92, 0xCF);   /* 2: kernel ma'lumot (ring 0) */
    gdt_set(3, 0, 0xFFFFFFFF, 0xFA, 0xCF);   /* 3: dastur kodi    (ring 3)  */
    gdt_set(4, 0, 0xFFFFFFFF, 0xF2, 0xCF);   /* 4: dastur ma'lumot (ring 3) */

    gdt_flush((uint32_t)&gdt_pointer);
}
