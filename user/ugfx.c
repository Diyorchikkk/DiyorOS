/*
 * ugfx.c — dasturlar uchun kichik grafika kutubxonasi (v0.7).
 * Kernel dagi gfx.c ning soddalashtirilgan ko'rinishi (ring 3 da ishlaydi).
 */
#include "libc.h"
#include "ugfx.h"

extern const unsigned char font8x16[128][16];

int cv_init(canvas_t *c, int w, int h)
{
    c->px = malloc((size_t)w * (size_t)h * 4);
    c->w = w;
    c->h = h;
    return c->px != 0;
}

void cv_fill(canvas_t *c, int x, int y, int w, int h, unsigned color)
{
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > c->w) w = c->w - x;
    if (y + h > c->h) h = c->h - y;
    for (int row = y; row < y + h; row++)
        for (int col = x; col < x + w; col++)
            c->px[row * c->w + col] = color;
}

void cv_darken(canvas_t *c, int x, int y, int w, int h)
{
    for (int row = y; row < y + h && row < c->h; row++)
        for (int col = x; col < x + w && col < c->w; col++)
            if (row >= 0 && col >= 0)
                c->px[row * c->w + col] = (c->px[row * c->w + col] >> 1) & 0x7F7F7F;
}

void cv_text_scaled(canvas_t *c, int x, int y, const char *s, int scale, unsigned color)
{
    for (; *s; s++, x += 8 * scale) {
        unsigned char ch = (unsigned char)*s;
        const unsigned char *g = font8x16[ch < 128 ? ch : '?'];
        for (int row = 0; row < 16; row++)
            for (int col = 0; col < 8; col++)
                if (g[row] & (0x80 >> col))
                    cv_fill(c, x + col * scale, y + row * scale, scale, scale, color);
    }
}

void cv_text(canvas_t *c, int x, int y, const char *s, unsigned color)
{
    cv_text_scaled(c, x, y, s, 1, color);
}

int cv_text_width(const char *s)
{
    return (int)strlen(s) * 8;
}

char *utoa(unsigned v, char *buf)
{
    char tmp[12];
    int n = 0;
    do {
        tmp[n++] = (char)('0' + v % 10);
        v /= 10;
    } while (v);
    for (int i = 0; i < n; i++)
        buf[i] = tmp[n - 1 - i];
    buf[n] = 0;
    return buf;
}
