/*
 * cmd_memory.c — xotira bilan bog'liq shell buyruqlari (v0.2).
 *
 *   mem        — umumiy holat: RAM, frame lar, paging, heap
 *   memmap     — BIOS bergan xotira xaritasi
 *   alloc N    — heap dan N bayt ajratish (kmalloc)
 *   free ADDR  — ajratilgan xotirani qaytarish (kfree)
 *   heap       — heap dagi barcha bloklar ro'yxati
 *   memtest    — heap ni avtomatik sinash
 *   translate  — virtual manzilni fizik manzilga aylantirish
 *   pagefault  — ataylab Page Fault chaqirish
 */
#include <stdint.h>
#include <stdbool.h>
#include "shell.h"
#include "kernel.h"
#include "memory.h"
#include "kprintf.h"
#include "vga.h"
#include "string.h"

/* "123" yoki "0x1F" ko'rinishidagi sonni o'qish */
static bool parse_number(const char *s, uint32_t *out)
{
    while (*s == ' ')
        s++;
    uint32_t v = 0;
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        s += 2;
        if (!*s)
            return false;
        for (; *s && *s != ' '; s++) {
            char c = *s;
            uint32_t d;
            if (c >= '0' && c <= '9') d = c - '0';
            else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
            else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
            else return false;
            v = (v << 4) | d;
        }
    } else {
        if (*s < '0' || *s > '9')
            return false;
        for (; *s && *s != ' '; s++) {
            if (*s < '0' || *s > '9')
                return false;
            v = v * 10 + (*s - '0');
        }
    }
    *out = v;
    return true;
}

static void title(const char *text)
{
    uint8_t old = vga_get_color();
    vga_set_color(VGA_LIGHT_CYAN, VGA_BLACK);
    kprintf("%s\n", text);
    vga_set_color(old & 0x0F, old >> 4);
}

void cmd_mem(const char *args)
{
    (void)args;
    uint32_t total = pmm_total_frames();
    uint32_t used  = pmm_used_frames();
    heap_stats_t hs;
    heap_get_stats(&hs);

    title("Fizik xotira (PMM)");
    kprintf("  RAM hajmi:        %u MB\n", pmm_memory_top() / (1024 * 1024));
    kprintf("  Frame lar (4 KB): jami %u, band %u, bo'sh %u\n", total, used, total - used);
    kprintf("  Bo'sh xotira:     %u KB\n", (total - used) * 4);
    kprintf("  Kernel:           0x%x - 0x%x (%u KB)\n",
            (uint32_t)kernel_start, (uint32_t)kernel_end,
            (uint32_t)(kernel_end - kernel_start) / 1024);

    title("Virtual xotira (paging)");
    kprintf("  Page Directory:   0x%x (CR3)\n", paging_directory_address());
    kprintf("  Identity map:     0x1000 - 0x%x (virtual = fizik)\n", paging_identity_limit());

    title("Kernel heap");
    kprintf("  Hudud:            0x%x - 0x%x (%u KB)\n", hs.start, hs.end, (hs.end - hs.start) / 1024);
    kprintf("  Band:             %u bayt, %u blok\n", hs.used_bytes, hs.used_blocks);
    kprintf("  Bo'sh:            %u bayt, %u blok\n", hs.free_bytes, hs.free_blocks);
}

void cmd_memmap(const char *args)
{
    (void)args;
    kprintf("BIOS xotira xaritasi:\n");
    kprintf("  Boshlanish   Oxiri        Hajm        Turi\n");
    for (int i = 0; i < pmm_region_count(); i++) {
        const mem_region_t *r = pmm_region(i);
        uint32_t size = r->length;
        kprintf("  0x%08x - 0x%08x  ", r->base, r->base + r->length - 1);
        if (size >= 1024 * 1024)
            kprintf("%6u MB   ", size / (1024 * 1024));
        else
            kprintf("%6u KB   ", size / 1024);
        uint8_t old = vga_get_color();
        vga_set_color(r->type == 1 ? VGA_LIGHT_GREEN : VGA_YELLOW, VGA_BLACK);
        kprintf("%s\n", r->type == 1 ? "bo'sh RAM" : "band (BIOS/qurilmalar)");
        vga_set_color(old & 0x0F, old >> 4);
    }
}

void cmd_alloc(const char *args)
{
    uint32_t size;
    if (!parse_number(args, &size) || size == 0 || size > 64 * 1024 * 1024) {
        kprintf("Foydalanish: alloc <bayt>   masalan: alloc 100\n");
        return;
    }
    uint8_t *p = kmalloc(size);
    if (!p) {
        kprintf("Xotira yetmadi!\n");
        return;
    }
    memset(p, 0xAB, size);               /* xotiraga haqiqatan yozish mumkinligini tekshiramiz */
    uint32_t phys = 0;
    paging_translate((uint32_t)p, &phys);
    kprintf("Ajratildi: %u bayt, virtual 0x%x -> fizik 0x%x\n", size, (uint32_t)p, phys);
    kprintf("Bo'shatish uchun: free 0x%x\n", (uint32_t)p);
}

void cmd_free(const char *args)
{
    uint32_t addr;
    if (!parse_number(args, &addr)) {
        kprintf("Foydalanish: free <manzil>   masalan: free 0xD0000018\n");
        return;
    }
    heap_stats_t before, after;
    heap_get_stats(&before);
    kfree((void *)addr);
    heap_get_stats(&after);
    if (after.used_blocks < before.used_blocks)
        kprintf("Bo'shatildi: 0x%x\n", addr);
}

void cmd_heap(const char *args)
{
    (void)args;
    heap_dump();
}

/* Oddiy tasodifiy sonlar generatori (LCG) */
static uint32_t rng_state = 12345;
static uint32_t rng(void)
{
    rng_state = rng_state * 1103515245u + 12345u;
    return rng_state >> 16;
}

void cmd_memtest(const char *args)
{
    (void)args;
    enum { N = 64 };
    uint8_t *ptr[N];
    uint32_t size[N];
    uint32_t frames_before = pmm_used_frames();
    heap_stats_t before;
    heap_get_stats(&before);          /* sinovdan oldingi holat */

    kprintf("1) %d ta tasodifiy hajmli blok ajratilmoqda...\n", N);
    for (int i = 0; i < N; i++) {
        size[i] = rng() % 3000 + 1;
        ptr[i] = kmalloc(size[i]);
        if (!ptr[i]) {
            kprintf("   XATO: kmalloc NULL qaytardi\n");
            return;
        }
        memset(ptr[i], (uint8_t)i, size[i]);
    }

    kprintf("2) Juft raqamlilari bo'shatilib, qayta ajratilmoqda...\n");
    for (int i = 0; i < N; i += 2)
        kfree(ptr[i]);
    for (int i = 0; i < N; i += 2) {
        size[i] = rng() % 3000 + 1;
        ptr[i] = kmalloc(size[i]);
        if (!ptr[i]) {
            kprintf("   XATO: kmalloc NULL qaytardi\n");
            return;
        }
        memset(ptr[i], (uint8_t)(i + 100), size[i]);
    }

    kprintf("3) Ma'lumotlar buzilmaganini tekshirish...\n");
    int errors = 0;
    for (int i = 0; i < N; i++) {
        uint8_t expected = (i % 2 == 0) ? (uint8_t)(i + 100) : (uint8_t)i;
        for (uint32_t j = 0; j < size[i]; j++)
            if (ptr[i][j] != expected) {
                errors++;
                break;
            }
    }

    kprintf("4) Hammasi bo'shatilmoqda...\n");
    for (int i = 0; i < N; i++)
        kfree(ptr[i]);

    heap_stats_t hs;
    heap_get_stats(&hs);
    /* Sinovdan keyin heap avvalgi holatiga qaytishi kerak: band bloklar soni
       o'zgarmaydi, bo'sh bloklar ko'paymaydi (ko'pi bilan heap oxirida bittasi qo'shiladi) */
    bool merged = (hs.used_blocks == before.used_blocks &&
                   hs.free_blocks <= before.free_blocks + 1);

    uint8_t old = vga_get_color();
    if (errors == 0 && merged) {
        vga_set_color(VGA_LIGHT_GREEN, VGA_BLACK);
        kprintf("NATIJA: muvaffaqiyatli. Ma'lumotlar to'g'ri, bo'sh bloklar qayta birlashdi.\n");
    } else {
        vga_set_color(VGA_LIGHT_RED, VGA_BLACK);
        kprintf("NATIJA: xato! Buzilgan bloklar: %d, bo'sh bloklar: %u (oldin %u)\n",
                errors, hs.free_blocks, before.free_blocks);
    }
    vga_set_color(old & 0x0F, old >> 4);
    kprintf("Heap hajmi: %u KB, ishlatilgan yangi frame lar: %u\n",
            (hs.end - hs.start) / 1024, pmm_used_frames() - frames_before);
}

void cmd_translate(const char *args)
{
    uint32_t virt, phys;
    if (!parse_number(args, &virt)) {
        kprintf("Foydalanish: translate <manzil>   masalan: translate 0xD0000000\n");
        return;
    }
    uint32_t pd = virt >> 22, pt = (virt >> 12) & 0x3FF, off = virt & 0xFFF;
    kprintf("Virtual 0x%08x = PD[%u] -> PT[%u] -> +0x%x\n", virt, pd, pt, off);
    if (paging_translate(virt, &phys))
        kprintf("Fizik manzil: 0x%08x\n", phys);
    else
        kprintf("Bu manzil xaritada yo'q (unga murojaat Page Fault beradi)\n");
}

void cmd_pagefault(const char *args)
{
    uint32_t addr = 0xA0000000;          /* xaritaga kiritilmagan manzil */
    if (strcmp(args, "kod") == 0) {
        /* v0.6.1: kernel kodi faqat o'qish uchun — ustiga yozish Page Fault beradi */
        volatile uint8_t *code = (volatile uint8_t *)(uint32_t)cmd_pagefault;
        kprintf("Kernel kodiga (0x%x) yozishga urinish... (Exception #14 kutilmoqda)\n", (uint32_t)code);
        *code = 0x90;
        kprintf("Yozildi (bu chiqmasligi kerak edi: kod himoyalanmagan)\n");
        return;
    }
    if (strcmp(args, "null") == 0)
        addr = 0;
    kprintf("0x%x manzilidan o'qishga urinish... (Exception #14 kutilmoqda)\n", addr);
    volatile uint32_t *p = (volatile uint32_t *)addr;
    uint32_t value = *p;
    kprintf("O'qildi: 0x%x (bu chiqmasligi kerak edi)\n", value);
}
