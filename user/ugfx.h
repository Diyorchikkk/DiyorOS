/*
 * ugfx.h — dasturlar uchun kichik grafika kutubxonasi (v0.7).
 *
 * Dastur ekranga to'g'ridan-to'g'ri tega olmaydi. U o'z xotirasidagi
 * piksellar massiviga (canvas) chizadi va win_update() bilan kernelga
 * beradi; kernel uni oynaga ko'chiradi. Shrift kernel bilan bir xil.
 */
#ifndef UGFX_H
#define UGFX_H

#define RGB(r, g, b) ((unsigned)(((r) << 16) | ((g) << 8) | (b)))

typedef struct {
    unsigned *px;
    int       w, h;
} canvas_t;

int  cv_init(canvas_t *c, int w, int h);       /* malloc bilan; 0 — xotira yetmadi */
void cv_fill(canvas_t *c, int x, int y, int w, int h, unsigned color);
void cv_darken(canvas_t *c, int x, int y, int w, int h);
void cv_text(canvas_t *c, int x, int y, const char *s, unsigned color);
void cv_text_scaled(canvas_t *c, int x, int y, const char *s, int scale, unsigned color);
int  cv_text_width(const char *s);

/* Son -> matn (printf ning kichik o'rnini bosuvchi) */
char *utoa(unsigned v, char *buf);

#endif
