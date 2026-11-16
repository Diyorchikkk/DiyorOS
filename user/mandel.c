/*
 * mandel.c — Mandelbrot to'plami (v0.7): ring 3 dagi grafik dastur.
 *
 * Har bir piksel uchun z = z^2 + c formulasi takrorlanadi; z cheksizlikka
 * "qochib ketsa" — nuqta to'plamdan tashqarida, necha qadamda qochgani
 * rangni belgilaydi. Hisob double sonlarda — FPU da bajariladi (v0.6.1 dagi
 * FPU kontekst almashtirish shu yerda kerak bo'ladi).
 *
 * Dastur ekranga TEGMAYDI: o'z xotirasidagi massivga chizadi va
 * win_update() syscall i bilan kernelga beradi. Kernel ko'rsatkichni
 * tekshirib, piksellarni oynaga ko'chiradi.
 *
 *   chap tugma — shu nuqtaga 2 marta yaqinlashish
 *   o'ng tugma — 2 marta uzoqlashish
 *   R — boshlang'ich ko'rinish, Q yoki Esc — chiqish
 */
#include "libc.h"
#include "ugfx.h"

#define MW 480
#define MH 360
#define BAR_H 22
#define MAX_ITER 128

static canvas_t cv;
static int win;
static double center_x = -0.6, center_y = 0.0;
static double step = 3.2 / MW;             /* bitta piksel necha birlikka teng */
static int zoom = 1;

enum { NOTHING, REDRAW, QUIT };

static unsigned color_of(int i)
{
    if (i >= MAX_ITER)
        return 0;
    double t = (double)i / MAX_ITER;
    /* Silliq palitra (Bernshteyn ko'phadlari): to'q ko'k -> to'q sariq */
    int r = (int)(9.0 * (1 - t) * t * t * t * 255);
    int g = (int)(15.0 * (1 - t) * (1 - t) * t * t * 255);
    int b = (int)(8.5 * (1 - t) * (1 - t) * (1 - t) * t * 255);
    return RGB(r > 255 ? 255 : r, g > 255 ? 255 : g, b > 255 ? 255 : b);
}

static void draw_bar(int done_rows)
{
    char buf[16];
    cv_fill(&cv, 0, MH, MW, BAR_H, RGB(30, 34, 42));
    cv_text(&cv, 6, MH + 3, "Zoom x", RGB(200, 210, 225));
    cv_text(&cv, 54, MH + 3, utoa((unsigned)zoom, buf), RGB(255, 255, 255));
    if (done_rows < MH) {
        cv_text(&cv, 140, MH + 3, "hisoblanmoqda...", RGB(250, 200, 70));
    } else {
        cv_text(&cv, 140, MH + 3, "chap: yaqin  o'ng: uzoq  R: qayta", RGB(150, 160, 175));
    }
}

/* Navbatdagi hodisalarni o'qish. REDRAW — ko'rinish o'zgardi, QUIT — chiqish */
static int poll(void)
{
    struct gui_event ev;
    int action = NOTHING, r;
    while ((r = win_event(win, &ev)) > 0) {
        if (ev.type == GUI_EV_CLOSE)
            return QUIT;
        if (ev.type == GUI_EV_KEY) {
            if (ev.key == 'q' || ev.key == KEY_ESC)
                return QUIT;
            if (ev.key == 'r') {
                center_x = -0.6; center_y = 0.0; step = 3.2 / MW; zoom = 1;
                action = REDRAW;
            }
        }
        if (ev.type == GUI_EV_MOUSE_DOWN && ev.y < MH) {
            center_x += (ev.x - MW / 2) * step;
            center_y += (ev.y - MH / 2) * step;
            if (ev.buttons & 1) {
                step /= 2;
                zoom *= 2;
            } else if (zoom > 1) {
                step *= 2;
                zoom /= 2;
            }
            action = REDRAW;
        }
    }
    return r < 0 ? QUIT : action;          /* oyna yopilgan bo'lsa */
}

/* Butun rasmni hisoblash. Har 12 qatorda ekranni yangilab, hodisalarni tekshiradi */
static int render(void)
{
    for (int y = 0; y < MH; y++) {
        double ci = center_y + (y - MH / 2) * step;
        for (int x = 0; x < MW; x++) {
            double cr = center_x + (x - MW / 2) * step;
            double zr = 0, zi = 0;
            int i = 0;
            while (i < MAX_ITER && zr * zr + zi * zi < 4.0) {
                double t = zr * zr - zi * zi + cr;
                zi = 2 * zr * zi + ci;
                zr = t;
                i++;
            }
            cv.px[y * MW + x] = color_of(i);
        }
        if (y % 12 == 11) {
            draw_bar(y + 1);
            win_update(win, cv.px);
            int a = poll();
            if (a != NOTHING)
                return a;
        }
    }
    draw_bar(MH);
    win_update(win, cv.px);
    return NOTHING;
}

int main(void)
{
    win = win_create("Mandelbrot (ring 3, FPU)", MW, MH + BAR_H);
    if (win < 0) {
        printf("mandel: grafik rejim kerak. Avval 'gui' buyrug'ini yozing.\n");
        return 1;
    }
    if (!cv_init(&cv, MW, MH + BAR_H)) {
        printf("mandel: xotira yetmadi\n");
        return 1;
    }
    cv_fill(&cv, 0, 0, MW, MH + BAR_H, 0);

    int action = REDRAW;
    for (;;) {
        if (action == QUIT)
            break;
        if (action == REDRAW) {
            action = render();
            continue;
        }
        sleep(20);                         /* hodisa kutish: protsessorni band qilmaymiz */
        action = poll();
    }
    win_close(win);
    return 0;
}
