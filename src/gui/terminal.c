/*
 * terminal.c — grafik terminal oynasi (v0.7).
 *
 * Shell grafik rejim haqida hech narsa bilmaydi: u avvalgidek 80x25 lik
 * matn kataklariga yozadi (vga.c). Grafik rejimda bu kataklar oddiy
 * xotiradagi massivda turadi. Terminal oynasi har aylanishda tekshiradi:
 * massiv o'zgarganmi (vga_generation)? O'zgargan bo'lsa — faqat o'zgargan
 * kataklarni shrift bilan piksellarga chizadi.
 *
 * Har bir katak: belgi (ASCII) + rang bayti (pastki 4 bit — matn,
 * yuqori 4 bit — fon), ya'ni VGA ning 16 ta rangi. Ularni zamonaviyroq
 * ko'rinadigan RGB ranglarga moslashtiramiz (palitra).
 */
#include "gui.h"
#include "vga.h"
#include "drivers.h"
#include "string.h"
#include "memory.h"

#define COLS VGA_WIDTH
#define ROWS VGA_HEIGHT
#define BLINK_TICKS 50

static const uint32_t palette[16] = {
    RGB(16, 20, 26),    RGB(46, 91, 186),   RGB(63, 163, 77),   RGB(42, 161, 179),
    RGB(194, 59, 59),   RGB(163, 71, 186),  RGB(192, 138, 40),  RGB(200, 204, 208),
    RGB(106, 112, 120), RGB(92, 141, 240),  RGB(107, 214, 107), RGB(95, 215, 232),
    RGB(240, 106, 106), RGB(224, 123, 232), RGB(242, 209, 75),  RGB(255, 255, 255),
};

typedef struct {
    uint16_t cells[COLS * ROWS];     /* ekranda chizilgan holat */
    uint16_t now[COLS * ROWS];       /* vga.c dan olingan yangi holat */
    uint32_t generation;
    int      cursor_pos;             /* chizilgan kursor joyi (-1 — yo'q) */
    bool     blink_on;
    uint32_t blink_tick;
    bool     first;
} term_t;

static window_t *term_window = 0;

static void draw_cell(window_t *w, int pos, uint16_t cell, bool cursor)
{
    int x = (pos % COLS) * FONT_W, y = (pos / COLS) * FONT_H;
    uint8_t attr = (uint8_t)(cell >> 8);
    uint32_t fg = palette[attr & 0x0F], bg = palette[(attr >> 4) & 0x0F];
    gfx_char(&w->surf, x, y, (char)(cell & 0xFF), fg, bg);
    if (cursor)
        gfx_fill(&w->surf, x, y + FONT_H - 3, FONT_W, 2, fg);   /* tagiga chiziq — kursor */
}

static void term_frame(window_t *w)
{
    term_t *t = w->data;
    uint32_t gen = vga_generation();
    uint32_t now_tick = wm_ticks();
    bool blink_changed = false;
    if (now_tick - t->blink_tick >= BLINK_TICKS) {
        t->blink_tick = now_tick;
        t->blink_on = !t->blink_on;
        blink_changed = true;
    }
    if (gen == t->generation && !blink_changed && !t->first)
        return;
    t->generation = gen;

    int cursor_pos;
    bool cursor_visible;
    vga_snapshot(t->now, &cursor_pos, &cursor_visible);
    int new_cursor = (cursor_visible && t->blink_on) ? cursor_pos : -1;

    /* Faqat o'zgargan kataklar va kursor turgan (yoki turgan) kataklar */
    int min_pos = COLS * ROWS, max_pos = -1;
    for (int i = 0; i < COLS * ROWS; i++) {
        bool cursor_here = (i == new_cursor), cursor_was = (i == t->cursor_pos);
        if (!t->first && t->now[i] == t->cells[i] && cursor_here == cursor_was)
            continue;
        t->cells[i] = t->now[i];
        draw_cell(w, i, t->cells[i], cursor_here);
        if (i < min_pos) min_pos = i;
        if (i > max_pos) max_pos = i;
    }
    t->cursor_pos = new_cursor;
    t->first = false;
    if (max_pos >= 0) {
        int r0 = min_pos / COLS, r1 = max_pos / COLS;
        wm_invalidate_rect(w, rect_make(0, r0 * FONT_H, COLS * FONT_W, (r1 - r0 + 1) * FONT_H));
    }
}

static void term_mouse(window_t *w, int type, int x, int y, int buttons)
{
    (void)w; (void)type; (void)x; (void)y; (void)buttons;    /* bosilganda faqat faollashadi */
}

/* G'ildirak: matn rejimidagidek tarixni varaqlash (shell buni tugma sifatida oladi) */
static void term_wheel(window_t *w, int dz)
{
    (void)w;
    char key = dz < 0 ? KEY_SCROLL_UP : KEY_SCROLL_DOWN;
    for (int i = 0; i < 3; i++)
        keyboard_inject(key);
}

/* Terminal oynasi yopilsa ham shell ishlashda davom etadi (u vga.c ga yozaveradi).
   Menyu yoki ikonka orqali qayta ochilganda hamma narsa yangidan chiziladi */
static void term_destroy(window_t *w)
{
    kfree(w->data);
    if (term_window == w)
        term_window = 0;
}

static const window_ops_t term_ops = {
    .mouse = term_mouse,
    .wheel = term_wheel,
    .frame = term_frame,
    .destroy = term_destroy,
};

bool app_is_terminal(const window_t *w)
{
    return w && w->ops == &term_ops;
}

void app_terminal_open(void)
{
    if (term_window)
        return;
    term_t *t = kmalloc(sizeof(term_t));
    if (!t)
        return;
    memset(t, 0, sizeof(*t));
    t->first = true;
    t->cursor_pos = -1;
    t->blink_on = true;
    window_t *w = wm_create("Terminal - shell", COLS * FONT_W, ROWS * FONT_H, &term_ops, t);
    if (!w) {
        kfree(t);
        return;
    }
    w->x = 300;
    w->y = 40;
    wm_focus(w);                     /* yangi joyda qayta chizilsin */
    term_window = w;
}
