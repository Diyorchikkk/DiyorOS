/*
 * fb.h — framebuffer: grafik rejim drayveri (v0.7).
 */
#ifndef FB_H
#define FB_H

#include <stdint.h>
#include <stdbool.h>

/* Framebuffer kernelning virtual xotirasida shu manzilga joylashadi.
   Sahifa jadvallari yuklanishda oldindan yaratiladi (paging.c) */
#define FB_VIRT     0xE0000000u
#define FB_MAX_SIZE (16u * 1024 * 1024)

typedef struct {
    bool      available;     /* grafik rejimga o'tish mumkinmi (adapter topildimi) */
    bool      active;        /* hozir grafik rejimdami */
    uint32_t  phys;          /* framebuffer ning fizik manzili (PCI BAR0) */
    uint32_t  vram_bytes;    /* videokarta xotirasi */
    uint32_t  width, height, bpp;
    uint32_t  pitch;         /* bitta qatorning baytlardagi uzunligi */
    uint32_t *pixels;        /* FB_VIRT */
    uint16_t  bga_version;
} fb_info_t;

bool             fb_detect(void);                       /* yuklanishda: adapter bormi */
const fb_info_t *fb_info(void);
bool             fb_set_mode(uint32_t w, uint32_t h);   /* matn -> grafik (32 bit rang) */
void             fb_text_mode(void);                    /* grafik -> matn (VGA 80x25) */
void             fb_emergency_text(void);               /* panic uchun: hech narsani kutmasdan matnga */

#endif
