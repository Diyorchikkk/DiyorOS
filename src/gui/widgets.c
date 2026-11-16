/*
 * widgets.c — tugmalar va ikonkalar (v0.7).
 *
 * Ikonkalar rasm fayl emas: har biri bir nechta to'rtburchak, doira va
 * chiziqdan kod bilan "chiziladi". Shunda ular kernel hajmini deyarli
 * oshirmaydi va har qanday rangda chizilishi mumkin.
 */
#include "gui.h"

void ui_button(surface_t *s, int x, int y, int w, int h, const char *label, bool pressed)
{
    gfx_round_rect(s, x, y, w, h, RGB(160, 168, 180));
    gfx_round_rect(s, x + 1, y + 1, w - 2, h - 2, pressed ? RGB(195, 210, 235) : COL_BUTTON);
    if (!pressed)
        gfx_hline(s, x + 3, y + 1, w - 6, RGB(250, 251, 253));
    int tx = x + (w - gfx_text_width(label)) / 2;
    int ty = y + (h - FONT_H) / 2;
    gfx_text(s, tx, ty, label, COL_TEXT, GFX_TRANSPARENT);
}

/* DiyorOS logotipi: to'q sariq-qizil gradientli kvadrat ichida qalin "D" harfi.
   size — kvadrat tomoni; harf masshtabi o'lchamga qarab tanlanadi */
void ui_logo(surface_t *s, int x, int y, int size)
{
    gfx_gradient_v(s, x, y, size, size, RGB(255, 176, 60), RGB(232, 84, 60));
    int scale = size >= 48 ? 3 : size >= 32 ? 2 : 1;
    int tx = x + (size - FONT_W * scale) / 2, ty = y + (size - FONT_H * scale) / 2;
    for (int dx = 0; dx < scale; dx++)              /* qalin: siljitib bir necha marta */
        gfx_text_scaled(s, tx + dx, ty, "D", scale, RGB(255, 255, 255));
}

static void icon_page(surface_t *s, int x, int y, uint32_t body)
{
    gfx_fill(s, x + 6, y + 2, 16, 28, RGB(120, 128, 140));
    gfx_fill(s, x + 7, y + 3, 14, 26, body);
    gfx_fill(s, x + 21, y + 2, 5, 28, RGB(120, 128, 140));      /* o'ng tomon */
    gfx_fill(s, x + 21, y + 8, 4, 21, body);
    gfx_fill(s, x + 20, y + 3, 1, 6, RGB(120, 128, 140));       /* buklangan burchak */
    gfx_fill(s, x + 21, y + 7, 4, 1, RGB(120, 128, 140));
}

void ui_icon(surface_t *s, int x, int y, int kind)
{
    switch (kind) {
    case ICON_TERMINAL:
        gfx_round_rect(s, x, y + 2, 32, 28, RGB(70, 78, 92));
        gfx_fill(s, x + 2, y + 8, 28, 20, RGB(22, 26, 32));
        gfx_fill(s, x + 4, y + 4, 3, 2, RGB(240, 90, 80));
        gfx_fill(s, x + 9, y + 4, 3, 2, RGB(250, 200, 70));
        gfx_text(s, x + 5, y + 10, ">_", RGB(110, 220, 110), GFX_TRANSPARENT);
        break;
    case ICON_FILES:
    case ICON_FOLDER:
        gfx_round_rect(s, x + 1, y + 5, 13, 6, RGB(214, 150, 30));
        gfx_round_rect(s, x + 1, y + 8, 30, 20, RGB(214, 150, 30));
        gfx_round_rect(s, x + 1, y + 11, 30, 17, RGB(248, 196, 64));
        gfx_hline(s, x + 3, y + 12, 26, RGB(252, 220, 120));
        break;
    case ICON_FILE:
        icon_page(s, x, y, RGB(252, 252, 252));
        for (int i = 0; i < 5; i++)
            gfx_hline(s, x + 10, y + 11 + i * 4, 11, RGB(170, 178, 190));
        break;
    case ICON_PROGRAM:
        gfx_round_rect(s, x + 2, y + 4, 28, 24, RGB(36, 130, 80));
        gfx_fill(s, x + 4, y + 9, 24, 17, RGB(46, 160, 100));
        gfx_text(s, x + 4, y + 9, "ELF", RGB(255, 255, 255), GFX_TRANSPARENT);
        break;
    case ICON_IMAGE:
        gfx_fill(s, x + 2, y + 4, 28, 24, RGB(120, 128, 140));
        gfx_fill(s, x + 3, y + 5, 26, 22, RGB(150, 200, 245));
        gfx_circle_fill(s, x + 22, y + 11, 3, RGB(250, 210, 70));
        {
            /* Tepalik: doiraning faqat rasm ichidagi qismi (kesish to'rtburchagi bilan) */
            rect_t old = s->clip, inner;
            if (rect_intersect(old, rect_make(x + 3, y + 5, 26, 22), &inner)) {
                s->clip = inner;
                gfx_circle_fill(s, x + 11, y + 30, 11, RGB(70, 160, 80));
                s->clip = old;
            }
        }
        break;
    case ICON_MONITOR:
        gfx_round_rect(s, x, y + 2, 32, 24, RGB(70, 78, 92));
        gfx_fill(s, x + 2, y + 4, 28, 20, RGB(22, 26, 32));
        gfx_fill(s, x + 5, y + 16, 4, 6, RGB(90, 200, 120));
        gfx_fill(s, x + 11, y + 10, 4, 12, RGB(90, 200, 120));
        gfx_fill(s, x + 17, y + 13, 4, 9, RGB(250, 200, 70));
        gfx_fill(s, x + 23, y + 7, 4, 15, RGB(240, 100, 90));
        gfx_fill(s, x + 12, y + 26, 8, 3, RGB(70, 78, 92));
        gfx_fill(s, x + 8, y + 29, 16, 2, RGB(70, 78, 92));
        break;
    case ICON_PAINT:
        gfx_circle_fill(s, x + 16, y + 16, 14, RGB(150, 110, 70));
        gfx_circle_fill(s, x + 16, y + 16, 13, RGB(236, 210, 170));
        gfx_circle_fill(s, x + 22, y + 22, 4, RGB(150, 110, 70));    /* barmoq teshigi */
        gfx_circle_fill(s, x + 22, y + 22, 3, RGB(196, 166, 126));
        gfx_circle_fill(s, x + 9, y + 12, 3, RGB(220, 50, 50));
        gfx_circle_fill(s, x + 16, y + 8, 3, RGB(50, 110, 220));
        gfx_circle_fill(s, x + 23, y + 11, 3, RGB(50, 170, 70));
        gfx_circle_fill(s, x + 10, y + 21, 3, RGB(240, 200, 40));
        break;
    case ICON_MANDEL:
        gfx_round_rect(s, x, y + 1, 32, 30, RGB(10, 10, 40));
        gfx_circle_fill(s, x + 18, y + 16, 9, RGB(240, 140, 40));
        gfx_circle_fill(s, x + 18, y + 16, 7, RGB(120, 40, 160));
        gfx_circle_fill(s, x + 18, y + 16, 5, RGB(0, 0, 0));
        gfx_circle_fill(s, x + 9, y + 16, 4, RGB(240, 140, 40));
        gfx_circle_fill(s, x + 9, y + 16, 2, RGB(0, 0, 0));
        break;
    case ICON_SNAKE:
        gfx_round_rect(s, x, y + 1, 32, 30, RGB(30, 60, 30));
        for (int i = 0; i < 4; i++)
            gfx_fill(s, x + 4 + i * 6, y + 18, 5, 5, RGB(120, 220, 90));
        gfx_fill(s, x + 22, y + 12, 5, 5, RGB(120, 220, 90));
        gfx_fill(s, x + 22, y + 6, 5, 5, RGB(160, 250, 120));
        gfx_circle_fill(s, x + 9, y + 9, 3, RGB(230, 60, 60));
        break;
    case ICON_ABOUT:
        gfx_circle_fill(s, x + 16, y + 16, 14, RGB(40, 98, 190));
        gfx_fill(s, x + 14, y + 13, 4, 11, RGB(255, 255, 255));
        gfx_fill(s, x + 14, y + 7, 4, 4, RGB(255, 255, 255));
        break;
    }
}
