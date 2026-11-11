/*
 * pmm.c — Physical Memory Manager (fizik xotira menejeri).
 *
 * RAM ni 4 KB lik bo'laklarga — "frame"larga bo'lamiz. Har bir frame uchun
 * BITMAP da bitta bit bor:  1 — band,  0 — bo'sh.
 * 128 MB RAM = 32768 frame = 32768 bit = atigi 4 KB bitmap.
 *
 * Qayerdan bilamiz qaysi RAM bo'sh? Bootloader BIOS dan "xotira xaritasi"ni
 * (memory map) olib beradi. Unda: "0x0–0x9FC00 — bo'sh, 0xF0000 — BIOS (band),
 * 0x100000–0x7FE0000 — bo'sh ..." kabi ma'lumot bor.
 *
 * Boshlanishda hammasini BAND deb belgilaymiz, keyin xaritadagi bo'sh joylarni
 * bo'shatamiz, so'ng birinchi 1 MB va kernelning o'zini qayta band qilamiz.
 */
#include "memory.h"
#include "kernel.h"
#include "string.h"

#define MAX_MEMORY   0x40000000u                  /* 1 GB gacha qo'llab-quvvatlaymiz */
#define MAX_FRAMES   (MAX_MEMORY / PAGE_SIZE)
#define MAX_REGIONS  32

static uint32_t bitmap[MAX_FRAMES / 32];
static uint32_t total_frames = 0;
static uint32_t used_frames  = 0;
static uint32_t memory_top   = 0;
static uint32_t search_hint  = 0;                 /* qidiruvni shu joydan boshlaymiz */

static mem_region_t regions[MAX_REGIONS];
static int region_count = 0;

static inline void frame_set(uint32_t f)   { bitmap[f / 32] |=  (1u << (f % 32)); }
static inline void frame_clear(uint32_t f) { bitmap[f / 32] &= ~(1u << (f % 32)); }
static inline bool frame_test(uint32_t f)  { return bitmap[f / 32] & (1u << (f % 32)); }

/* [start, end) oralig'idagi to'liq frame larni bo'sh deb belgilash */
static void free_range(uint64_t start, uint64_t end)
{
    if (end > MAX_MEMORY)
        end = MAX_MEMORY;
    uint64_t first = (start + PAGE_SIZE - 1) / PAGE_SIZE;   /* yuqoriga yaxlitlash */
    uint64_t last  = end / PAGE_SIZE;                       /* pastga yaxlitlash */
    for (uint64_t f = first; f < last; f++)
        frame_clear((uint32_t)f);
}

/* [start, end) oralig'iga tegadigan barcha frame larni band qilish */
static void reserve_range(uint32_t start, uint32_t end)
{
    for (uint32_t f = start / PAGE_SIZE; f < (end + PAGE_SIZE - 1) / PAGE_SIZE; f++)
        if (f < MAX_FRAMES)
            frame_set(f);
}

void pmm_init(multiboot_info_t *mbi)
{
    memset(bitmap, 0xFF, sizeof(bitmap));    /* boshida hammasi band */

    if (mbi->flags & MULTIBOOT_FLAG_MMAP) {
        uint32_t addr = mbi->mmap_addr;
        uint32_t end  = mbi->mmap_addr + mbi->mmap_length;

        while (addr < end) {
            multiboot_mmap_entry_t *e = (multiboot_mmap_entry_t *)addr;

            if (region_count < MAX_REGIONS && e->addr < 0x100000000ull) {
                uint64_t len = e->len;
                if (e->addr + len > 0x100000000ull)
                    len = 0x100000000ull - e->addr;
                regions[region_count].base   = (uint32_t)e->addr;
                regions[region_count].length = (uint32_t)len;
                regions[region_count].type   = e->type;
                region_count++;
            }

            if (e->type == 1) {                    /* ishlatsa bo'ladigan RAM */
                free_range(e->addr, e->addr + e->len);
                uint64_t top = e->addr + e->len;
                if (top > MAX_MEMORY)
                    top = MAX_MEMORY;
                if (top > memory_top)
                    memory_top = (uint32_t)top;
            }
            addr += e->size + 4;                   /* keyingi yozuvga o'tish */
        }
    } else {
        /* Xarita yo'q bo'lsa — 1 MB dan yuqoridagi xotirani ishlatamiz */
        memory_top = 0x100000 + mbi->mem_upper * 1024;
        free_range(0x100000, memory_top);
    }

    total_frames = memory_top / PAGE_SIZE;

    /* Birinchi 1 MB (BIOS, VGA, bootloader ma'lumotlari) va kernelning o'zi — band */
    reserve_range(0, 0x100000);
    reserve_range((uint32_t)kernel_start, (uint32_t)kernel_end);

    used_frames = 0;
    for (uint32_t f = 0; f < total_frames; f++)
        if (frame_test(f))
            used_frames++;
}

uint32_t pmm_alloc_frame(void)
{
    for (uint32_t i = 0; i < total_frames; i++) {
        uint32_t f = (search_hint + i) % total_frames;
        if (bitmap[f / 32] == 0xFFFFFFFF) {        /* 32 ta frame birdan band — tez o'tkazib yuboramiz */
            i += 31 - (f % 32);
            continue;
        }
        if (!frame_test(f)) {
            frame_set(f);
            used_frames++;
            search_hint = f + 1;
            return f * PAGE_SIZE;
        }
    }
    return 0;                                      /* xotira tugadi */
}

void pmm_free_frame(uint32_t addr)
{
    uint32_t f = addr / PAGE_SIZE;
    if (f >= total_frames || !frame_test(f))
        return;                                    /* allaqachon bo'sh — e'tiborsiz qoldiramiz */
    frame_clear(f);
    used_frames--;
    if (f < search_hint)
        search_hint = f;
}

uint32_t pmm_total_frames(void)  { return total_frames; }
uint32_t pmm_used_frames(void)   { return used_frames; }
uint32_t pmm_memory_top(void)    { return memory_top; }
int      pmm_region_count(void)  { return region_count; }

const mem_region_t *pmm_region(int i)
{
    return (i >= 0 && i < region_count) ? &regions[i] : 0;
}
