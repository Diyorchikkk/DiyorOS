/*
 * gfx.c — 2D grafika kutubxonasi (v0.7).
 *
 * Grafik interfeysdagi hamma narsa (oynalar, tugmalar, matn, sichqoncha
 * ko'rsatkichi) shu fayldagi bir nechta oddiy amaldan yig'iladi: piksel,
 * to'rtburchak, chiziq, harf va nusxalash. Har bir amal sirtning kesish
 * to'rtburchagini (clip) hisobga oladi.
 */
#include "gfx.h"
#include "memory.h"
#include "string.h"

/* ------------------------------- To'rtburchaklar ------------------------------- */

rect_t rect_make(int x, int y, int w, int h)
{
    rect_t r = { x, y, w, h };
    return r;
}

bool rect_empty(rect_t r)
{
    return r.w <= 0 || r.h <= 0;
}

bool rect_intersect(rect_t a, rect_t b, rect_t *out)
{
    int x0 = a.x > b.x ? a.x : b.x;
    int y0 = a.y > b.y ? a.y : b.y;
    int x1 = (a.x + a.w) < (b.x + b.w) ? (a.x + a.w) : (b.x + b.w);
    int y1 = (a.y + a.h) < (b.y + b.h) ? (a.y + a.h) : (b.y + b.h);
    *out = rect_make(x0, y0, x1 - x0, y1 - y0);
    return x1 > x0 && y1 > y0;
}

/* Ikkalasini qamraydigan eng kichik to'rtburchak */
rect_t rect_union(rect_t a, rect_t b)
{
    if (rect_empty(a))
        return b;
    if (rect_empty(b))
        return a;
    int x0 = a.x < b.x ? a.x : b.x;
    int y0 = a.y < b.y ? a.y : b.y;
    int x1 = (a.x + a.w) > (b.x + b.w) ? (a.x + a.w) : (b.x + b.w);
    int y1 = (a.y + a.h) > (b.y + b.h) ? (a.y + a.h) : (b.y + b.h);
    return rect_make(x0, y0, x1 - x0, y1 - y0);
}

bool rect_contains(rect_t r, int x, int y)
{
    return x >= r.x && y >= r.y && x < r.x + r.w && y < r.y + r.h;
}

/* ----------------------------------- Sirt ----------------------------------- */

bool surface_init(surface_t *s, int w, int h)
{
    s->px = kmalloc((size_t)w * (size_t)h * 4);
    if (!s->px) {
        s->w = s->h = 0;
        return false;
    }
    s->w = w;
    s->h = h;
    s->stride = w;
    surface_reset_clip(s);
    memset(s->px, 0, (size_t)w * (size_t)h * 4);
    return true;
}

void surface_free(surface_t *s)
{
    if (s->px)
        kfree(s->px);
    s->px = 0;
    s->w = s->h = 0;
}

void surface_wrap(surface_t *s, uint32_t *px, int w, int h, int stride)
{
    s->px = px;
    s->w = w;
    s->h = h;
    s->stride = stride;
    surface_reset_clip(s);
}

void surface_set_clip(surface_t *s, rect_t r)
{
    rect_t full = rect_make(0, 0, s->w, s->h);
    if (!rect_intersect(full, r, &s->clip))
        s->clip = rect_make(0, 0, 0, 0);
}

void surface_reset_clip(surface_t *s)
{
    s->clip = rect_make(0, 0, s->w, s->h);
}

/* ---------------------------------- Chizish ---------------------------------- */

void gfx_pixel(surface_t *s, int x, int y, uint32_t c)
{
    if (rect_contains(s->clip, x, y))
        s->px[y * s->stride + x] = c;
}

uint32_t gfx_get(const surface_t *s, int x, int y)
{
    if (x < 0 || y < 0 || x >= s->w || y >= s->h)
        return 0;
    return s->px[y * s->stride + x];
}

/* To'ldirilgan to'rtburchak: avval kesamiz, keyin har qatorni tez to'ldiramiz */
void gfx_fill(surface_t *s, int x, int y, int w, int h, uint32_t c)
{
    rect_t r;
    if (!rect_intersect(s->clip, rect_make(x, y, w, h), &r))
        return;
    for (int row = r.y; row < r.y + r.h; row++) {
        uint32_t *p = &s->px[row * s->stride + r.x];
        int n = r.w;
        /* "rep stosl" — protsessor bir buyruq bilan n ta 4-baytli sonni yozadi */
        __asm__ volatile ("cld; rep stosl" : "+D"(p), "+c"(n) : "a"(c) : "memory", "cc");
    }
}

void gfx_hline(surface_t *s, int x, int y, int w, uint32_t c)
{
    gfx_fill(s, x, y, w, 1, c);
}

void gfx_vline(surface_t *s, int x, int y, int h, uint32_t c)
{
    gfx_fill(s, x, y, 1, h, c);
}

void gfx_rect(surface_t *s, int x, int y, int w, int h, uint32_t c)
{
    gfx_hline(s, x, y, w, c);
    gfx_hline(s, x, y + h - 1, w, c);
    gfx_vline(s, x, y, h, c);
    gfx_vline(s, x + w - 1, y, h, c);
}

/* Burchaklari 2 pikselga "kesilgan" to'rtburchak — tugmalar uchun */
void gfx_round_rect(surface_t *s, int x, int y, int w, int h, uint32_t c)
{
    gfx_fill(s, x + 2, y, w - 4, h, c);
    gfx_fill(s, x + 1, y + 1, 1, h - 2, c);
    gfx_fill(s, x + w - 2, y + 1, 1, h - 2, c);
    gfx_fill(s, x, y + 2, 1, h - 4, c);
    gfx_fill(s, x + w - 1, y + 2, 1, h - 4, c);
}

/*
 * Bresenham algoritmi: faqat butun sonlar bilan to'g'ri chiziq chizish.
 * Har qadamda x (yoki y) bittaga siljiydi, "xato" yig'indisi yarim
 * pikseldan oshsa — ikkinchi o'q ham siljiydi.
 */
void gfx_line(surface_t *s, int x0, int y0, int x1, int y1, uint32_t c)
{
    int dx = x1 > x0 ? x1 - x0 : x0 - x1;
    int dy = y1 > y0 ? y0 - y1 : y1 - y0;      /* manfiy */
    int sx = x0 < x1 ? 1 : -1;
    int sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    for (int guard = 0; guard < 8192; guard++) {
        gfx_pixel(s, x0, y0, c);
        if (x0 == x1 && y0 == y1)
            break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

void gfx_circle_fill(surface_t *s, int cx, int cy, int r, uint32_t c)
{
    for (int y = -r; y <= r; y++) {
        int half = 0;
        while ((half + 1) * (half + 1) + y * y <= r * r)
            half++;
        gfx_hline(s, cx - half, cy + y, 2 * half + 1, c);
    }
}

/* Qalin chiziq: chiziq bo'ylab doirachalar (Chizish dasturidagi mo'yqalam) */
void gfx_thick_line(surface_t *s, int x0, int y0, int x1, int y1, int r, uint32_t c)
{
    int dx = x1 - x0, dy = y1 - y0;
    int steps = (dx < 0 ? -dx : dx) > (dy < 0 ? -dy : dy) ? (dx < 0 ? -dx : dx) : (dy < 0 ? -dy : dy);
    if (steps == 0) {
        gfx_circle_fill(s, x0, y0, r, c);
        return;
    }
    for (int i = 0; i <= steps; i++)
        gfx_circle_fill(s, x0 + dx * i / steps, y0 + dy * i / steps, r, c);
}

/* Ikki rang orasida vertikal o'tish (gradient): har qator uchun rang chiziqli hisoblanadi */
void gfx_gradient_v(surface_t *s, int x, int y, int w, int h, uint32_t top, uint32_t bottom)
{
    if (h <= 0)
        return;
    int r0 = (top >> 16) & 0xFF, g0 = (top >> 8) & 0xFF, b0 = top & 0xFF;
    int r1 = (bottom >> 16) & 0xFF, g1 = (bottom >> 8) & 0xFF, b1 = bottom & 0xFF;
    int from = s->clip.y > y ? s->clip.y - y : 0;
    int to = (s->clip.y + s->clip.h) < (y + h) ? (s->clip.y + s->clip.h) - y : h;
    for (int i = from; i < to; i++) {
        uint32_t c = RGB(r0 + (r1 - r0) * i / h, g0 + (g1 - g0) * i / h, b0 + (b1 - b0) * i / h);
        gfx_hline(s, x, y + i, w, c);
    }
}

/* Soya: har bir rang komponentini 2 marta kamaytirish (bit siljitish bilan, tez) */
void gfx_darken(surface_t *s, int x, int y, int w, int h)
{
    rect_t r;
    if (!rect_intersect(s->clip, rect_make(x, y, w, h), &r))
        return;
    for (int row = r.y; row < r.y + r.h; row++) {
        uint32_t *p = &s->px[row * s->stride + r.x];
        for (int i = 0; i < r.w; i++)
            p[i] = (p[i] >> 1) & 0x7F7F7F;
    }
}

void gfx_blit(surface_t *dst, int dx, int dy, const surface_t *src, int sx, int sy, int w, int h)
{
    /* Manba chegarasi */
    if (sx < 0) { dx -= sx; w += sx; sx = 0; }
    if (sy < 0) { dy -= sy; h += sy; sy = 0; }
    if (sx + w > src->w) w = src->w - sx;
    if (sy + h > src->h) h = src->h - sy;

    /* Maqsad chegarasi (clip) */
    rect_t r;
    if (!rect_intersect(dst->clip, rect_make(dx, dy, w, h), &r))
        return;
    sx += r.x - dx;
    sy += r.y - dy;
    for (int row = 0; row < r.h; row++)
        memcpy(&dst->px[(r.y + row) * dst->stride + r.x],
               &src->px[(sy + row) * src->stride + sx], (size_t)r.w * 4);
}

/* ----------------------------------- Matn ----------------------------------- */

void gfx_char(surface_t *s, int x, int y, char ch, uint32_t fg, uint32_t bg)
{
    uint8_t code = (uint8_t)ch;
    const uint8_t *glyph = font8x16[code < 128 ? code : '?'];
    rect_t cell = rect_make(x, y, FONT_W, FONT_H), vis;
    if (!rect_intersect(s->clip, cell, &vis))
        return;
    bool whole = vis.w == FONT_W && vis.h == FONT_H;     /* to'liq ko'rinadi — tez yo'l */
    for (int row = 0; row < FONT_H; row++) {
        uint8_t bits = glyph[row];
        if (whole) {
            uint32_t *p = &s->px[(y + row) * s->stride + x];
            for (int col = 0; col < FONT_W; col++) {
                if (bits & (0x80 >> col))
                    p[col] = fg;
                else if (bg != GFX_TRANSPARENT)
                    p[col] = bg;
            }
        } else {
            for (int col = 0; col < FONT_W; col++) {
                if (bits & (0x80 >> col))
                    gfx_pixel(s, x + col, y + row, fg);
                else if (bg != GFX_TRANSPARENT)
                    gfx_pixel(s, x + col, y + row, bg);
            }
        }
    }
}

void gfx_text(surface_t *s, int x, int y, const char *text, uint32_t fg, uint32_t bg)
{
    for (; *text; text++, x += FONT_W)
        gfx_char(s, x, y, *text, fg, bg);
}

/* Qalin matn: har bir harf bir piksel o'ngga siljitib ikki marta chiziladi */
void gfx_text_bold(surface_t *s, int x, int y, const char *text, uint32_t fg)
{
    gfx_text(s, x, y, text, fg, GFX_TRANSPARENT);
    gfx_text(s, x + 1, y, text, fg, GFX_TRANSPARENT);
}

/* Kattalashtirilgan matn: har bir piksel scale x scale kvadrat bo'lib chiziladi */
void gfx_text_scaled(surface_t *s, int x, int y, const char *text, int scale, uint32_t fg)
{
    for (; *text; text++, x += FONT_W * scale) {
        uint8_t code = (uint8_t)*text;
        const uint8_t *glyph = font8x16[code < 128 ? code : '?'];
        if (!rect_intersect(s->clip, rect_make(x, y, FONT_W * scale, FONT_H * scale), &(rect_t){0}))
            continue;
        for (int row = 0; row < FONT_H; row++)
            for (int col = 0; col < FONT_W; col++)
                if (glyph[row] & (0x80 >> col))
                    gfx_fill(s, x + col * scale, y + row * scale, scale, scale, fg);
    }
}

int gfx_text_width(const char *text)
{
    return (int)strlen(text) * FONT_W;
}

/* ------------------------------------ BMP ------------------------------------ */
/*
 * BMP — eng sodda rasm formati: 54 baytlik sarlavha + piksellar (siqilmagan).
 * Qatorlar PASTDAN yuqoriga saqlanadi, har piksel 3 bayt (ko'k, yashil, qizil),
 * har qator 4 baytga karrali bo'lishi uchun oxiriga nollar qo'shiladi.
 * Windows, macOS, brauzerlar — hammasi ocha oladi.
 */
static void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put32(uint8_t *p, uint32_t v) { put16(p, (uint16_t)v); put16(p + 2, (uint16_t)(v >> 16)); }
static uint16_t get16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t get32(const uint8_t *p) { return get16(p) | ((uint32_t)get16(p + 2) << 16); }

static uint32_t row_bytes(int w)
{
    return ((uint32_t)w * 3 + 3) & ~3u;
}

uint32_t gfx_bmp_size(int w, int h)
{
    return 54 + row_bytes(w) * (uint32_t)h;
}

void gfx_bmp_encode(const surface_t *s, uint8_t *out)
{
    uint32_t rb = row_bytes(s->w), size = gfx_bmp_size(s->w, s->h);
    memset(out, 0, 54);
    out[0] = 'B';
    out[1] = 'M';
    put32(out + 2, size);
    put32(out + 10, 54);               /* piksellar qayerdan boshlanadi */
    put32(out + 14, 40);               /* BITMAPINFOHEADER hajmi */
    put32(out + 18, (uint32_t)s->w);
    put32(out + 22, (uint32_t)s->h);
    put16(out + 26, 1);                /* tekisliklar */
    put16(out + 28, 24);               /* bit / piksel */
    put32(out + 34, rb * (uint32_t)s->h);
    put32(out + 38, 2835);             /* 72 DPI */
    put32(out + 42, 2835);
    for (int y = 0; y < s->h; y++) {
        uint8_t *row = out + 54 + (uint32_t)(s->h - 1 - y) * rb;
        memset(row, 0, rb);
        for (int x = 0; x < s->w; x++) {
            uint32_t c = s->px[y * s->stride + x];
            row[x * 3 + 0] = (uint8_t)c;
            row[x * 3 + 1] = (uint8_t)(c >> 8);
            row[x * 3 + 2] = (uint8_t)(c >> 16);
        }
    }
}

bool gfx_bmp_decode(const uint8_t *d, uint32_t len, surface_t *out)
{
    if (len < 54 || d[0] != 'B' || d[1] != 'M')
        return false;
    uint32_t offset = get32(d + 10);
    int32_t w = (int32_t)get32(d + 18), h = (int32_t)get32(d + 22);
    uint16_t bpp = get16(d + 28);
    uint32_t compression = get32(d + 30);
    bool top_down = h < 0;
    if (top_down)
        h = -h;
    if ((bpp != 24 && bpp != 32) || (compression != 0 && compression != 3) ||
        w <= 0 || h <= 0 || w > 2048 || h > 2048)
        return false;
    uint32_t rb = bpp == 24 ? row_bytes(w) : (uint32_t)w * 4;
    if (offset > len || rb * (uint32_t)h > len - offset)
        return false;                  /* fayl qisqa yoki buzilgan */
    if (!surface_init(out, w, h))
        return false;
    for (int y = 0; y < h; y++) {
        const uint8_t *row = d + offset + (uint32_t)(top_down ? y : h - 1 - y) * rb;
        for (int x = 0; x < w; x++) {
            const uint8_t *p = row + x * (bpp / 8);
            out->px[y * w + x] = RGB(p[2], p[1], p[0]);
        }
    }
    return true;
}
