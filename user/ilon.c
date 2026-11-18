/*
 * ilon.c — "Ilon" o'yini (v0.7): ring 3 dagi grafik dastur.
 *
 * O'yin tsikli (game loop) — har qanday o'yinning asosi:
 *   1) hodisalarni o'qish (strelkalar)
 *   2) vaqti kelgan bo'lsa — holatni yangilash (ilon bir katak siljiydi)
 *   3) chizish va win_update() bilan kernelga berish
 *   4) qisqa uxlash (sleep) — protsessorni boshqa dasturlarga bo'shatish
 *
 *   strelkalar yoki W A S D — yo'nalish, P — pauza, R — qayta, Q / Esc — chiqish
 */
#include "libc.h"
#include "ugfx.h"

#define CELL 20
#define GW   24
#define GH   18
#define TOP  32
#define WW   (GW * CELL)
#define WH   (TOP + GH * CELL)

typedef struct { int x, y; } point_t;

static canvas_t cv;
static int win;
static point_t snake[GW * GH];
static int length, dir_x, dir_y, next_x, next_y;
static point_t food;
static int score, best = 0;
static int alive, paused;
static unsigned step_ms;

static int on_snake(int x, int y)
{
    for (int i = 0; i < length; i++)
        if (snake[i].x == x && snake[i].y == y)
            return 1;
    return 0;
}

static void place_food(void)
{
    do {
        food.x = (int)(rand() % GW);
        food.y = (int)(rand() % GH);
    } while (on_snake(food.x, food.y));
}

static void reset(void)
{
    length = 4;
    for (int i = 0; i < length; i++) {
        snake[i].x = GW / 2 - i;
        snake[i].y = GH / 2;
    }
    dir_x = next_x = 1;
    dir_y = next_y = 0;
    score = 0;
    alive = 1;
    paused = 0;
    step_ms = 140;
    place_food();
}

static void step(void)
{
    dir_x = next_x;
    dir_y = next_y;
    point_t head = { snake[0].x + dir_x, snake[0].y + dir_y };
    if (head.x < 0 || head.y < 0 || head.x >= GW || head.y >= GH || on_snake(head.x, head.y)) {
        alive = 0;                         /* devorga yoki o'ziga urildi */
        if (score > best)
            best = score;
        return;
    }
    int ate = head.x == food.x && head.y == food.y;
    if (ate && length < GW * GH)
        length++;
    for (int i = length - 1; i > 0; i--)
        snake[i] = snake[i - 1];
    snake[0] = head;
    if (ate) {
        score += 10;
        if (step_ms > 60)
            step_ms -= 5;                  /* har bir olma — biroz tezroq */
        place_food();
    }
}

static void draw(void)
{
    char buf[16];
    cv_fill(&cv, 0, 0, WW, TOP, RGB(30, 34, 42));
    cv_text(&cv, 10, 8, "Ochko:", RGB(200, 210, 225));
    cv_text(&cv, 66, 8, utoa((unsigned)score, buf), RGB(255, 255, 255));
    cv_text(&cv, 130, 8, "Rekord:", RGB(200, 210, 225));
    cv_text(&cv, 194, 8, utoa((unsigned)best, buf), RGB(250, 200, 70));
    cv_text(&cv, WW - 210, 8, "P - pauza, Q - chiqish", RGB(130, 140, 155));

    for (int y = 0; y < GH; y++)               /* shaxmat ko'rinishidagi maydon */
        for (int x = 0; x < GW; x++)
            cv_fill(&cv, x * CELL, TOP + y * CELL, CELL, CELL,
                    (x + y) & 1 ? RGB(30, 58, 36) : RGB(34, 64, 40));

    cv_fill(&cv, food.x * CELL + 3, TOP + food.y * CELL + 3, CELL - 6, CELL - 6, RGB(230, 60, 60));
    cv_fill(&cv, food.x * CELL + 9, TOP + food.y * CELL + 1, 2, 4, RGB(110, 70, 30));

    for (int i = length - 1; i >= 0; i--) {
        unsigned c = i == 0 ? RGB(170, 250, 120) : (i & 1 ? RGB(110, 210, 80) : RGB(96, 196, 70));
        cv_fill(&cv, snake[i].x * CELL + 1, TOP + snake[i].y * CELL + 1, CELL - 2, CELL - 2, c);
    }
    /* Ko'zlar */
    cv_fill(&cv, snake[0].x * CELL + 5, TOP + snake[0].y * CELL + 5, 3, 3, 0);
    cv_fill(&cv, snake[0].x * CELL + 12, TOP + snake[0].y * CELL + 5, 3, 3, 0);

    if (!alive || paused) {
        cv_darken(&cv, 0, TOP, WW, GH * CELL);
        const char *title = alive ? "Pauza" : "O'yin tugadi";
        const char *hint = alive ? "P - davom etish" : "R - qayta boshlash,  Q - chiqish";
        cv_text_scaled(&cv, (WW - cv_text_width(title) * 3) / 2, TOP + 110, title, 3, RGB(255, 255, 255));
        cv_text(&cv, (WW - cv_text_width(hint)) / 2, TOP + 180, hint, RGB(220, 225, 235));
    }
    win_update(win, cv.px);
}

static void turn(int x, int y)
{
    if (x == -dir_x && y == -dir_y)
        return;                            /* orqaga burilib bo'lmaydi */
    next_x = x;
    next_y = y;
}

int main(void)
{
    win = win_create("Ilon (ring 3)", WW, WH);
    if (win < 0) {
        printf("ilon: grafik rejim kerak. Avval 'grafik' buyrug'ini yozing.\n");
        return 1;
    }
    if (!cv_init(&cv, WW, WH)) {
        printf("ilon: xotira yetmadi\n");
        return 1;
    }
    srand(uptime() * 2654435761u);
    reset();
    draw();

    unsigned last = uptime();
    for (;;) {
        struct gui_event ev;
        int r;
        while ((r = win_event(win, &ev)) > 0) {
            if (ev.type == GUI_EV_CLOSE)
                goto out;
            if (ev.type != GUI_EV_KEY)
                continue;
            int k = ev.key;
            if (k == 'q' || k == KEY_ESC) goto out;
            if (k == KEY_UP || k == 'w') turn(0, -1);
            if (k == KEY_DOWN || k == 's') turn(0, 1);
            if (k == KEY_LEFT || k == 'a') turn(-1, 0);
            if (k == KEY_RIGHT || k == 'd') turn(1, 0);
            if (k == 'p' && alive) { paused = !paused; draw(); }
            if (k == 'r' && !alive) { reset(); draw(); }
        }
        if (r < 0)
            break;                         /* oyna yopildi */
        unsigned now = uptime();
        if (alive && !paused && now - last >= step_ms) {
            last = now;
            step();
            draw();
        }
        sleep(10);
    }
out:
    win_close(win);
    return 0;
}
