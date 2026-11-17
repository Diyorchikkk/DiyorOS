/*
 * gfx.h — 2D grafika kutubxonasi (v0.7).
 *
 * "Sirt" (surface) — piksellar massivi: ekranning orqa buferi ham, har bir
 * oynaning ichki tasviri ham sirt. Barcha chizish funksiyalari sirtning
 * "kesish to'rtburchagi" (clip) dan tashqariga chiqmaydi: oyna chetidan
 * chiqib ketgan chiziq boshqa xotirani buzmaydi.
 *
 * Rang: 0x00RRGGBB (qizil, yashil, ko'k — har biri 0..255).
 */
#ifndef GFX_H
#define GFX_H

#include <stdint.h>
#include <stdbool.h>

#define RGB(r, g, b) ((uint32_t)(((r) << 16) | ((g) << 8) | (b)))
#define FONT_W 8
#define FONT_H 16
#define GFX_TRANSPARENT 0xFF000000u    /* matn foni uchun: "fon chizilmasin" */

typedef struct {
    int x, y, w, h;
} rect_t;

typedef struct {
    uint32_t *px;          /* piksellar */
    int       w, h;        /* o'lcham */
    int       stride;      /* bitta qatordagi piksellar soni (odatda w) */
    rect_t    clip;        /* faqat shu to'rtburchak ichiga chiziladi */
} surface_t;

extern const uint8_t font8x16[128][16];
extern const uint32_t vga_palette_rgb[16];   /* VGA ning 16 rangi RGB da */

/* Sirt */
bool     surface_init(surface_t *s, int w, int h);   /* kmalloc bilan */
void     surface_free(surface_t *s);
void     surface_wrap(surface_t *s, uint32_t *px, int w, int h, int stride);
void     surface_set_clip(surface_t *s, rect_t r);
void     surface_reset_clip(surface_t *s);

/* To'rtburchaklar */
rect_t   rect_make(int x, int y, int w, int h);
bool     rect_intersect(rect_t a, rect_t b, rect_t *out);
rect_t   rect_union(rect_t a, rect_t b);
bool     rect_empty(rect_t r);
bool     rect_contains(rect_t r, int x, int y);

/* Chizish */
void     gfx_pixel(surface_t *s, int x, int y, uint32_t c);
uint32_t gfx_get(const surface_t *s, int x, int y);
void     gfx_fill(surface_t *s, int x, int y, int w, int h, uint32_t c);
void     gfx_rect(surface_t *s, int x, int y, int w, int h, uint32_t c);      /* faqat chegara */
void     gfx_hline(surface_t *s, int x, int y, int w, uint32_t c);
void     gfx_vline(surface_t *s, int x, int y, int h, uint32_t c);
void     gfx_line(surface_t *s, int x0, int y0, int x1, int y1, uint32_t c);
void     gfx_circle_fill(surface_t *s, int cx, int cy, int r, uint32_t c);
void     gfx_thick_line(surface_t *s, int x0, int y0, int x1, int y1, int r, uint32_t c);
void     gfx_gradient_v(surface_t *s, int x, int y, int w, int h, uint32_t top, uint32_t bottom);
void     gfx_darken(surface_t *s, int x, int y, int w, int h);              /* soya effekti */
void     gfx_round_rect(surface_t *s, int x, int y, int w, int h, uint32_t c);   /* burchaklari silliq */

/* Nusxalash: src sirtning (sx, sy, w, h) qismini dst ning (dx, dy) joyiga */
void     gfx_blit(surface_t *dst, int dx, int dy, const surface_t *src, int sx, int sy, int w, int h);

/* Matn (8x16 shrift). bg = GFX_TRANSPARENT bo'lsa fon chizilmaydi */
void     gfx_char(surface_t *s, int x, int y, char ch, uint32_t fg, uint32_t bg);
void     gfx_text(surface_t *s, int x, int y, const char *text, uint32_t fg, uint32_t bg);
void     gfx_text_bold(surface_t *s, int x, int y, const char *text, uint32_t fg);
void     gfx_text_scaled(surface_t *s, int x, int y, const char *text, int scale, uint32_t fg);
int      gfx_text_width(const char *text);

/* Rasm faylini yozish (BMP, 24 bit) — Chizish dasturi uchun. Hajm qaytaradi */
uint32_t gfx_bmp_size(int w, int h);
void     gfx_bmp_encode(const surface_t *s, uint8_t *out);
bool     gfx_bmp_decode(const uint8_t *data, uint32_t len, surface_t *out);   /* 24/32 bit, siqilmagan */

#endif
