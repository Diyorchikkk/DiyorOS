/*
 * kheap.c — kernel heap: kmalloc() va kfree().
 *
 * PMM faqat 4 KB lik butun frame bera oladi. Lekin kernelga ko'pincha
 * 20 bayt, 300 bayt kabi kichik xotira kerak bo'ladi. Heap shu muammoni hal
 * qiladi: katta hududni kichik "bloklar"ga bo'lib beradi.
 *
 * Har bir blok oldida sarlavha (header) turadi:
 *
 *   [ header | foydalanuvchi ma'lumoti ][ header | ... ][ header | ... ]
 *
 * Bloklar ikki tomonlama bog'langan ro'yxat (linked list) hosil qiladi va
 * xotirada ketma-ket joylashgan.
 *
 * kmalloc: "First Fit" — birinchi mos keladigan bo'sh blokni topamiz.
 *          Blok juda katta bo'lsa — ikkiga bo'lamiz (split).
 *          Mos blok bo'lmasa — heap ni kengaytiramiz: PMM dan yangi frame
 *          olib, uni paging orqali heap oxiriga ulaymiz.
 * kfree:   blokni bo'sh deb belgilaymiz va qo'shni bo'sh bloklar bilan
 *          birlashtiramiz (coalescing) — aks holda xotira "maydalanib" ketadi
 *          (fragmentatsiya).
 *
 * HIMOYA (v0.6.1): har bir sarlavhada "sehrli raqam" bor. Kimdir o'z blokidan
 * tashqariga yozib yuborsa (buffer overflow), qo'shni blok sarlavhasi buziladi.
 * Ro'yxat bo'ylab yurganda buni sezsak — kernel panic: buzilgan heap bilan
 * ishlashda davom etish ma'lumotlarni yo'qotishga olib keladi.
 */
#include "memory.h"
#include "kprintf.h"
#include "string.h"
#include "vga.h"
#include "irq.h"
#include "klog.h"

#define BLOCK_MAGIC 0xD1B0B10Cu   /* sarlavha buzilmaganini tekshirish uchun belgi */
#define ALIGNMENT   8
#define MIN_SPLIT   16             /* bundan kichik qoldiq uchun blokni bo'lmaymiz */

typedef struct block {
    uint32_t magic;
    uint32_t size;                 /* foydalanuvchi qismining hajmi (sarlavhasiz) */
    uint32_t free;
    struct block *next;
    struct block *prev;
    uint32_t reserved;             /* sarlavha hajmini 24 baytga (8 ga karrali) tekislash */
} block_t;

#define HEADER_SIZE sizeof(block_t)

static block_t *first_block = 0;
static block_t *last_block  = 0;
static uint32_t heap_end = HEAP_START;   /* xaritaga ulangan hududning oxiri */

static inline uint32_t align_up(uint32_t v, uint32_t a)
{
    return (v + a - 1) & ~(a - 1);
}

static inline uint8_t *block_data(block_t *b)
{
    return (uint8_t *)b + HEADER_SIZE;
}

/* Heap ni kamida 'bytes' baytga kengaytirish. Yangi hudud boshini qaytaradi, 0 — xato */
static uint32_t heap_grow(uint32_t bytes)
{
    uint32_t old_end = heap_end;
    uint32_t new_end = align_up(heap_end + bytes, PAGE_SIZE);
    if (new_end > HEAP_MAX || new_end < old_end)
        return 0;

    for (uint32_t page = old_end; page < new_end; page += PAGE_SIZE) {
        uint32_t frame = pmm_alloc_frame();
        if (!frame || !paging_map(page, frame, PAGE_RW)) {
            /* Muvaffaqiyatsiz — shu paytgacha ulanganlarni qaytaramiz */
            for (uint32_t p = old_end; p < page; p += PAGE_SIZE) {
                uint32_t phys;
                if (paging_translate(p, &phys))
                    pmm_free_frame(phys);
                paging_unmap(p);
            }
            if (frame)
                pmm_free_frame(frame);
            return 0;
        }
    }
    heap_end = new_end;
    return old_end;
}

/* Sarlavha butunmi? Buzilgan bo'lsa — davom etib bo'lmaydi */
static inline void check_block(const block_t *b)
{
    if (b->magic != BLOCK_MAGIC)
        panic("heap buzilgan: 0x%x dagi blok sarlavhasi ustiga yozilgan (buffer overflow?)",
              (uint32_t)b);
}

void heap_init(void)
{
    uint32_t start = heap_grow(PAGE_SIZE);
    if (!start)
        panic("heap_init: xotira yetmadi");
    first_block = last_block = (block_t *)start;
    first_block->magic = BLOCK_MAGIC;
    first_block->size  = PAGE_SIZE - HEADER_SIZE;
    first_block->free  = 1;
    first_block->next  = 0;
    first_block->prev  = 0;
}

/* Blokni 'size' baytlik va qoldiq bo'sh blokka ajratish */
static void split_block(block_t *b, uint32_t size)
{
    if (b->size < size + HEADER_SIZE + MIN_SPLIT)
        return;

    block_t *rest = (block_t *)(block_data(b) + size);
    rest->magic = BLOCK_MAGIC;
    rest->size  = b->size - size - HEADER_SIZE;
    rest->free  = 1;
    rest->next  = b->next;
    rest->prev  = b;
    if (b->next)
        b->next->prev = rest;
    else
        last_block = rest;
    b->next = rest;
    b->size = size;
}

/* b va undan keyingi blok ikkalasi bo'sh bo'lsa — bittaga birlashtirish */
static void merge_with_next(block_t *b)
{
    block_t *n = b->next;
    if (!n || !b->free || !n->free)
        return;
    b->size += HEADER_SIZE + n->size;
    b->next = n->next;
    if (n->next)
        n->next->prev = b;
    else
        last_block = b;
    n->magic = 0;
}

static void *kmalloc_locked(size_t size);

/* v0.5: heap ni bir nechta vazifa ishlatadi — har bir chaqiruv uzilishlarsiz bajariladi */
void *kmalloc(size_t size)
{
    uint32_t f = irq_save();
    void *p = kmalloc_locked(size);
    irq_restore(f);
    return p;
}

static void *kmalloc_locked(size_t size)
{
    if (size == 0 || size > HEAP_MAX - HEAP_START)
        return 0;                         /* juda katta so'rov (align_up da toshib ketmasin) */
    uint32_t need = align_up((uint32_t)size, ALIGNMENT);

    /* 1) First Fit: birinchi mos bo'sh blok */
    for (block_t *b = first_block; b; b = b->next) {
        check_block(b);
        if (b->free && b->size >= need) {
            split_block(b, need);
            b->free = 0;
            return block_data(b);
        }
    }

    /* 2) Mos blok yo'q — heap ni kengaytiramiz */
    uint32_t grow = need + HEADER_SIZE;
    if (last_block->free)                 /* oxirgi blok bo'sh bo'lsa, uni cho'zamiz */
        grow -= last_block->size + HEADER_SIZE;
    uint32_t old_end = heap_end;
    if (!heap_grow(grow))
        return 0;                         /* xotira tugadi */
    uint32_t added = heap_end - old_end;

    if (last_block->free) {
        last_block->size += added;
    } else {
        block_t *nb = (block_t *)old_end;
        nb->magic = BLOCK_MAGIC;
        nb->size  = added - HEADER_SIZE;
        nb->free  = 1;
        nb->next  = 0;
        nb->prev  = last_block;
        last_block->next = nb;
        last_block = nb;
    }

    block_t *b = last_block;
    split_block(b, need);
    b->free = 0;
    return block_data(b);
}

static void kfree_locked(void *ptr);

void kfree(void *ptr)
{
    uint32_t f = irq_save();
    kfree_locked(ptr);
    irq_restore(f);
}

static void kfree_locked(void *ptr)
{
    if (!ptr)
        return;
    uint32_t addr = (uint32_t)ptr;
    if (addr < HEAP_START + HEADER_SIZE || addr >= heap_end) {
        kprintf("kfree: 0x%x heap ga tegishli emas\n", addr);
        return;
    }

    block_t *b = (block_t *)(addr - HEADER_SIZE);
    if (b->magic != BLOCK_MAGIC) {
        kprintf("kfree: 0x%x - noto'g'ri manzil (blok boshi emas)\n", addr);
        return;
    }
    if (b->free) {
        kprintf("kfree: 0x%x - ikki marta bo'shatish (double free)!\n", addr);
        return;
    }

    b->free = 1;
    merge_with_next(b);                   /* o'ngdagi qo'shni bilan */
    if (b->prev && b->prev->free)         /* chapdagi qo'shni bilan */
        merge_with_next(b->prev);
}

void heap_get_stats(heap_stats_t *s)
{
    memset(s, 0, sizeof(*s));
    uint32_t f = irq_save();              /* ro'yxat yurish paytida o'zgarmasin */
    s->start = HEAP_START;
    s->end   = heap_end;
    for (block_t *b = first_block; b; b = b->next) {
        check_block(b);
        if (b->free) {
            s->free_bytes += b->size;
            s->free_blocks++;
        } else {
            s->used_bytes += b->size;
            s->used_blocks++;
        }
    }
    irq_restore(f);
}

void heap_dump(void)
{
    int i = 0;
    uint32_t f = irq_save();
    kprintf("  #   Manzil       Hajm (bayt)  Holat\n");
    for (block_t *b = first_block; b; b = b->next, i++) {
        if (i >= 40) {
            kprintf("  ... (yana bloklar bor)\n");
            break;
        }
        kprintf("  %3d 0x%08x  %10u   ", i, (uint32_t)block_data(b), b->size);
        uint8_t old = vga_get_color();
        vga_set_color(b->free ? VGA_LIGHT_GREEN : VGA_YELLOW, VGA_BLACK);
        kprintf("%s\n", b->free ? "bo'sh" : "band");
        vga_set_color(old & 0x0F, old >> 4);
    }
    irq_restore(f);
}
