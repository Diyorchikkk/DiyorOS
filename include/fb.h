/*
 * fb.h — framebuffer: grafik rejim drayveri (v0.7).
 *
 * v0.9: ikki xil manba bo'lishi mumkin:
 *   1) Bochs VBE (QEMU/UTM videokartasi) — rejimni kernel o'zi almashtiradi (v0.7);
 *   2) BOOTLOADER bergan framebuffer (GRUB: UEFI GOP yoki BIOS VBE). Haqiqiy
 *      kompyuterlarda shu yagona yo'l: rejim yuklanishda bir marta o'rnatiladi,
 *      kernel uni almashtira olmaydi — matn konsoli ham shu ekranga chiziladi.
 */
#ifndef FB_H
#define FB_H

#include <stdint.h>
#include <stdbool.h>

/* Framebuffer kernelning virtual xotirasida shu manzilga joylashadi.
   Sahifa jadvallari yuklanishda oldindan yaratiladi (paging.c) */
#define FB_VIRT     0xE0000000u
#define FB_MAX_SIZE (64u * 1024 * 1024)   /* v0.9: 4K ekran ham sig'adi (3840x2160x4 = 33 MB) */

typedef struct {
    bool      available;     /* grafik rejimga o'tish mumkinmi (adapter topildimi) */
    bool      active;        /* hozir grafik rejimdami */
    uint32_t  phys;          /* framebuffer ning fizik manzili (PCI BAR0) */
    uint32_t  vram_bytes;    /* videokarta xotirasi */
    uint32_t  width, height, bpp;
    uint32_t  pitch;         /* bitta qatorning baytlardagi uzunligi */
    uint32_t *pixels;        /* FB_VIRT */
    uint16_t  bga_version;
    bool      boot;          /* v0.9: framebuffer ni bootloader bergan (rejimni o'zgartirib bo'lmaydi) */
    uint8_t   red_pos, green_pos, blue_pos;   /* piksel ichida ranglarning joyi (bit) */
} fb_info_t;

/* v0.9: bootloader bergan ekran (kernel_main boshida, paging dan oldin) */
void             fb_boot_setup(uint32_t phys, uint32_t w, uint32_t h, uint32_t pitch, uint32_t bpp,
                               uint8_t rpos, uint8_t gpos, uint8_t bpos);
bool             fb_boot_map(void);                     /* paging yoqilgach: FB_VIRT ga ulash */
bool             fb_detect(void);                       /* yuklanishda: adapter bormi */
const fb_info_t *fb_info(void);
bool             fb_set_mode(uint32_t w, uint32_t h);   /* matn -> grafik (32 bit rang) */
void             fb_text_mode(void);                    /* grafik -> matn (VGA 80x25) */
void             fb_emergency_text(void);               /* panic uchun: hech narsani kutmasdan matnga */

#endif
