/*
 * wm.c — oyna menejeri va kompozitor (v0.7).
 *
 * TUZILISHI
 *
 *   sichqoncha (IRQ12) --\
 *                         >-- kirish navbati --> wm oqimi --> oynalar, menyu, panel
 *   klaviatura (IRQ1) ---/                          |
 *                                                   v
 *   dasturlar (syscall) --> oyna sirtlari --> KOMPOZITSIYA --> orqa bufer --> framebuffer
 *
 * - Uzilishlar sichqoncha va klaviatura hodisalarini faqat navbatga qo'yadi
 *   (uzilish ichida uzoq ish qilinmaydi).
 * - "wm" — alohida kernel oqimi (ps da ko'rinadi). U ~100 marta soniyada
 *   uyg'onib, navbatni qayta ishlaydi, kerak bo'lsa ekranni qayta yig'adi.
 *   Sichqoncha yoki klaviatura hodisasi kelsa — darhol uyg'otiladi (task_wake).
 * - Har bir oynaning O'Z tasviri (sirti) bor. Dastur faqat o'z sirtiga
 *   chizadi; oyna menejeri ularni ustma-ust qo'yib, bitta tasvir yig'adi
 *   (kompozitsiya). Shuning uchun oynalar bir-birini "buzmaydi".
 *
 * IKKI BUFER (double buffering)
 * Hamma narsa avval oddiy xotiradagi "orqa bufer"ga chiziladi, keyin tayyor
 * natija bir martada framebuffer ga ko'chiriladi. To'g'ridan-to'g'ri ekranga
 * chizilsa, foydalanuvchi yarim chizilgan oynalarni ko'rardi (miltillash).
 *
 * IFLOS HUDUD (dirty rectangle)
 * Har safar butun ekranni (3 MB) qayta yig'ish qimmat. Shuning uchun
 * faqat o'zgargan joy belgilanadi: sichqoncha siljisa — ko'rsatkichning
 * eski va yangi joyi, terminalga harf chiqsa — o'sha qator. Kompozitsiya
 * faqat shu to'rtburchak ichida bajariladi.
 *
 * QULF
 * Oynalar ro'yxatini ham wm oqimi, ham dasturlar (syscall orqali) o'zgartiradi.
 * Bir vaqtda o'zgartirilmasligi uchun "wm" mutexi ishlatiladi (v0.8): band
 * bo'lsa, vazifa uxlaydi va qulf bo'shaganda navbat bilan uyg'otiladi.
 * Qulflar tartibi butun kernelda bir xil: wm -> vfs -> ata (teskarisi yo'q).
 */
#include "gui.h"
#include "sync.h"
#include "fb.h"
#include "vga.h"
#include "drivers.h"
#include "task.h"
#include "memory.h"
#include "string.h"
#include "kprintf.h"
#include "klog.h"
#include "irq.h"
#include "kernel.h"
#include "config.h"

/* ------------------------------- Holat ------------------------------- */

static bool running = false;
static volatile bool stop_requested = false;
static surface_t back;                     /* orqa bufer (1024x768) */
static surface_t screen;                   /* framebuffer ning o'zi */
static window_t windows[MAX_WINDOWS];
static window_t *zorder[MAX_WINDOWS];      /* pastdan yuqoriga */
static int zcount = 0;
static window_t *focus = 0;
static volatile bool focus_terminal = false;
static rect_t dirty;
static int next_id = 1;
static int cascade = 0;

static int mx = SCREEN_W / 2, my = SCREEN_H / 2;   /* sichqoncha ko'rsatkichi */
static uint8_t mbuttons = 0;
static window_t *drag_win = 0;
static int drag_dx, drag_dy;
static window_t *capture = 0;              /* tugma bosilgan oyna: qo'yib yuborilguncha hodisalar unga */

static bool menu_open = false;
static int menu_hover = -1;
static int icon_selected = -1;
static int last_click_icon = -1;
static uint32_t last_click_tick = 0;
static char clock_text[16] = "--:--:--";
static uint32_t last_second = 0;
static uint32_t tracked[8];                /* oyna menejeri ishga tushirgan dasturlar */

/* --------------------------- Kirish navbati --------------------------- */

enum { IN_MOUSE, IN_KEY };
typedef struct {
    uint8_t kind;
    uint8_t buttons;
    char    key;
    int8_t  dz;
    int16_t dx, dy;
} input_t;

#define INPUT_Q 256
static input_t inq[INPUT_Q];
static volatile uint32_t in_head = 0, in_tail = 0;

static void input_push(input_t e)
{
    uint32_t f = irq_save();
    /* Ketma-ket siljishlarni birlashtiramiz: navbat to'lib ketmasin */
    if (e.kind == IN_MOUSE && e.dz == 0 && in_head != in_tail) {
        input_t *last = &inq[(in_head + INPUT_Q - 1) % INPUT_Q];
        if (last->kind == IN_MOUSE && last->dz == 0 && last->buttons == e.buttons) {
            last->dx = (int16_t)(last->dx + e.dx);
            last->dy = (int16_t)(last->dy + e.dy);
            irq_restore(f);
            return;
        }
    }
    uint32_t next = (in_head + 1) % INPUT_Q;
    if (next != in_tail) {
        inq[in_head] = e;
        in_head = next;
    }
    irq_restore(f);
}

static bool input_pop(input_t *e)
{
    uint32_t f = irq_save();
    bool ok = in_head != in_tail;
    if (ok) {
        *e = inq[in_tail];
        in_tail = (in_tail + 1) % INPUT_Q;
    }
    irq_restore(f);
    return ok;
}

/*
 * Sichqoncha tezligi: 1..10, 5 — bir siljish birligi = bir piksel.
 * Siljish tezlikka ko'paytirilib 5 ga bo'linadi; bo'linishdan qolgan qoldiq
 * keyingi paketga qo'shiladi (sub-piksel aniqlik). Shunda sekin tezlikda ham
 * kursor "zinapoya" bo'lib emas, tekis yuradi.
 * v0.7 ning birinchi variantidagi tezlashtirish (d + d*|d|/8) olib tashlandi:
 * tez harakatda kursor sakrab ketardi.
 */
static volatile int mouse_speed = 5;
static int rem_x = 0, rem_y = 0;
static task_t *wm_task = 0;

static int scale_delta(int d, int *rem)
{
    int total = d * mouse_speed + *rem;
    int out = total / 5;
    *rem = total - out * 5;
    return out;
}

void gui_set_mouse_speed(int speed)
{
    if (speed < 1) speed = 1;
    if (speed > 10) speed = 10;
    mouse_speed = speed;
}

int gui_mouse_speed(void)
{
    return mouse_speed;
}

/* Uzilish ichida chaqiriladi (mouse.c) */
static void mouse_hook(int dx, int dy, int dz, uint8_t buttons)
{
    input_t e = { IN_MOUSE, buttons, 0, (int8_t)dz,
                  (int16_t)scale_delta(dx, &rem_x), (int16_t)scale_delta(dy, &rem_y) };
    input_push(e);
    task_wake(wm_task);                    /* tikni kutmasdan — darhol chizilsin */
}

/* Uzilish ichida chaqiriladi (keyboard.c). 1 — tugmani oyna menejeri oldi */
static int key_hook(char key)
{
    if (!running || focus_terminal || key == KEY_CTRL_C)
        return 0;                          /* terminal faol: tugma shell ga boradi */
    input_t e = { IN_KEY, 0, key, 0, 0, 0 };
    input_push(e);
    task_wake(wm_task);
    return 1;
}

/* -------------------------------- Qulf -------------------------------- */

/*
 * v0.8: haqiqiy mutex. v0.7 da "band bo'lsa — yield qilib qayta urinish"
 * (spin + yield) edi: kutayotgan vazifa har kvantda bekorga uyg'onardi.
 * Endi u uxlaydi va qulf bo'shaganda navbat bilan uyg'otiladi.
 * Oyna menejeri, user dasturlarning oyna syscall lari shu qulfni oladi.
 */
static mutex_t wm_mutex;
static bool    wm_mutex_ready = false;

static void wm_lock(void)
{
    if (!wm_mutex_ready) {                  /* birinchi chaqiruvda yaratiladi (bo'linmas) */
        uint32_t f = irq_save();
        if (!wm_mutex_ready) {
            mutex_init(&wm_mutex, "wm", false);
            wm_mutex_ready = true;
        }
        irq_restore(f);
    }
    if (mutex_lock(&wm_mutex) != SYNC_OK)
        panic("wm qulfida deadlock (qulflar tartibi buzilgan)");
}

static void wm_unlock(void)
{
    mutex_unlock(&wm_mutex);
}

/* ------------------------------ Geometriya ------------------------------ */

static rect_t frame_rect(const window_t *w)
{
    return rect_make(w->x, w->y, w->surf.w + 2, w->surf.h + TITLE_H + 1);
}

static rect_t frame_with_shadow(const window_t *w)
{
    rect_t f = frame_rect(w);
    f.w += 5;
    f.h += 5;
    return f;
}

static rect_t content_rect(const window_t *w)
{
    return rect_make(w->x + 1, w->y + TITLE_H, w->surf.w, w->surf.h);
}

static rect_t close_button(const window_t *w)
{
    rect_t f = frame_rect(w);
    return rect_make(f.x + f.w - 26, f.y + 4, 21, 18);
}

static rect_t min_button(const window_t *w)
{
    rect_t f = frame_rect(w);
    return rect_make(f.x + f.w - 50, f.y + 4, 21, 18);
}

static void invalidate_screen(rect_t r)
{
    rect_t clipped;
    if (rect_intersect(r, rect_make(0, 0, SCREEN_W, SCREEN_H), &clipped))
        dirty = rect_union(dirty, clipped);
}

static void invalidate_frame(const window_t *w)
{
    invalidate_screen(frame_with_shadow(w));
}

static rect_t taskbar_rect(void)
{
    return rect_make(0, SCREEN_H - TASKBAR_H, SCREEN_W, TASKBAR_H);
}

static rect_t start_button(void)
{
    return rect_make(4, SCREEN_H - TASKBAR_H + 4, 112, TASKBAR_H - 8);
}

static rect_t task_button(int k)
{
    return rect_make(124 + k * 154, SCREEN_H - TASKBAR_H + 4, 150, TASKBAR_H - 8);
}

/* ---------------------------- Menyu va ikonkalar ---------------------------- */

typedef struct {
    const char *label;
    int icon;
} item_t;

static const item_t desktop_icons[] = {
    { "Terminal",   ICON_TERMINAL },
    { "Fayllar",    ICON_FILES    },
    { "Monitor",    ICON_MONITOR  },
    { "Chizish",    ICON_PAINT    },
    { "Mandelbrot", ICON_MANDEL   },
    { "Ilon",       ICON_SNAKE    },
    { "Haqida",     ICON_ABOUT    },
};
#define ICON_COUNT_DESKTOP ((int)(sizeof(desktop_icons) / sizeof(desktop_icons[0])))

static const char *menu_items[] = {
    "Terminal", "Fayllar", "Tizim monitori", "Chizish", "Mandelbrot (ring 3)",
    "Ilon o'yini (ring 3)", "DiyorOS haqida", "-", "Matn rejimiga qaytish",
    "Qayta yuklash", "O'chirish",
};
#define MENU_COUNT ((int)(sizeof(menu_items) / sizeof(menu_items[0])))
#define MENU_W      240
#define MENU_ITEM_H 26
#define MENU_HEAD_H 40

static int menu_height(void)
{
    int h = MENU_HEAD_H + 6;
    for (int i = 0; i < MENU_COUNT; i++)
        h += menu_items[i][0] == '-' ? 9 : MENU_ITEM_H;
    return h + 4;
}

static rect_t menu_rect(void)
{
    int h = menu_height();
    return rect_make(4, SCREEN_H - TASKBAR_H - h - 2, MENU_W, h);
}

static rect_t menu_item_rect(int i)
{
    rect_t m = menu_rect();
    int y = m.y + MENU_HEAD_H + 6;
    for (int k = 0; k < i; k++)
        y += menu_items[k][0] == '-' ? 9 : MENU_ITEM_H;
    return rect_make(m.x + 4, y, m.w - 8, menu_items[i][0] == '-' ? 9 : MENU_ITEM_H);
}

static rect_t icon_rect(int i)
{
    return rect_make(16, 16 + i * 84, 84, 76);
}

/* Ish stoli yoki menyudagi dasturni ishga tushirish */
static void launch(int what)
{
    switch (what) {
    case 0: {                                  /* terminal bitta: borini oldinga chiqaramiz */
        for (int i = 0; i < MAX_WINDOWS; i++)
            if (windows[i].used && app_is_terminal(&windows[i])) {
                windows[i].minimized = false;
                wm_focus(&windows[i]);
                return;
            }
        app_terminal_open();
        break;
    }
    case 1: app_files_open("/home/diyorbek"); break;
    case 2: app_monitor_open(); break;
    case 3: app_paint_open(); break;
    case 4: app_run_program("/bin/mandel"); break;
    case 5: app_run_program("/bin/ilon"); break;
    case 6: app_about_open(); break;
    }
}

static void menu_action(int i)
{
    if (i < 7) {
        launch(i);
        return;
    }
    if (i == 8)
        gui_request_stop();
    else if (i == 9)
        power_reboot();
    else if (i == 10)
        power_off();
}

/* ------------------------------- Oynalar ------------------------------- */

static void set_focus(window_t *w)
{
    if (focus == w)
        return;
    if (focus)
        invalidate_frame(focus);
    focus = w;
    focus_terminal = w && app_is_terminal(w);
    if (w)
        invalidate_frame(w);
}

/* Eng yuqoridagi ko'rinib turgan oyna */
static window_t *top_visible(void)
{
    for (int i = zcount - 1; i >= 0; i--)
        if (!zorder[i]->minimized)
            return zorder[i];
    return 0;
}

static void raise(window_t *w)
{
    int i;
    for (i = 0; i < zcount && zorder[i] != w; i++)
        ;
    if (i == zcount)
        return;
    for (; i < zcount - 1; i++)
        zorder[i] = zorder[i + 1];
    zorder[zcount - 1] = w;
    invalidate_frame(w);
}

void wm_focus(window_t *w)
{
    if (!w)
        return;
    w->minimized = false;
    raise(w);
    set_focus(w);
}

window_t *wm_create(const char *title, int w, int h, const window_ops_t *ops, void *data)
{
    window_t *win = 0;
    for (int i = 0; i < MAX_WINDOWS; i++)
        if (!windows[i].used) {
            win = &windows[i];
            break;
        }
    if (!win)
        return 0;
    memset(win, 0, sizeof(*win));
    if (!surface_init(&win->surf, w, h))
        return 0;
    gfx_fill(&win->surf, 0, 0, w, h, COL_WINDOW_BG);
    win->used = true;
    win->id = next_id++;
    strlcpy(win->title, title, sizeof(win->title));
    win->ops = ops;
    win->data = data;

    /* Pog'onali joylashtirish: har yangi oyna oldingisidan biroz pastroqda */
    win->x = 130 + (cascade % 7) * 30;
    win->y = 30 + (cascade % 7) * 28;
    cascade++;
    rect_t f = frame_rect(win);
    if (win->x + f.w > SCREEN_W)
        win->x = SCREEN_W - f.w;
    if (win->y + f.h > SCREEN_H - TASKBAR_H)
        win->y = SCREEN_H - TASKBAR_H - f.h;
    if (win->x < 0) win->x = 0;
    if (win->y < 0) win->y = 0;

    zorder[zcount++] = win;
    set_focus(win);
    invalidate_frame(win);
    invalidate_screen(taskbar_rect());
    return win;
}

void wm_destroy(window_t *w)
{
    if (!w || !w->used)
        return;
    invalidate_frame(w);
    invalidate_screen(taskbar_rect());
    int i;
    for (i = 0; i < zcount && zorder[i] != w; i++)
        ;
    for (; i < zcount - 1; i++)
        zorder[i] = zorder[i + 1];
    if (zcount > 0)
        zcount--;
    if (capture == w) capture = 0;
    if (drag_win == w) drag_win = 0;
    if (w->ops && w->ops->destroy)
        w->ops->destroy(w);
    surface_free(&w->surf);
    w->used = false;
    if (focus == w) {
        focus = 0;
        set_focus(top_visible());
    }
}

void wm_invalidate(window_t *w)
{
    if (w && w->used && !w->minimized)
        invalidate_screen(content_rect(w));
}

void wm_invalidate_rect(window_t *w, rect_t r)
{
    if (!w || !w->used || w->minimized)
        return;
    r.x += w->x + 1;
    r.y += w->y + TITLE_H;
    rect_t c;
    if (rect_intersect(r, content_rect(w), &c))
        invalidate_screen(c);
}

void wm_set_title(window_t *w, const char *title)
{
    strlcpy(w->title, title, sizeof(w->title));
    invalidate_frame(w);
    invalidate_screen(taskbar_rect());
}

uint32_t wm_ticks(void)
{
    return timer_ticks();
}

void wm_track_process(uint32_t pid)
{
    for (int i = 0; i < 8; i++)
        if (!tracked[i]) {
            tracked[i] = pid;
            return;
        }
}

static void minimize(window_t *w)
{
    w->minimized = true;
    invalidate_frame(w);
    invalidate_screen(taskbar_rect());
    if (focus == w) {
        focus = 0;
        set_focus(top_visible());
    }
}

/* User oyna navbatiga hodisa qo'shish */
static void queue_push(window_t *w, int type, int x, int y, int key, int buttons)
{
    if (type == GUI_EV_MOUSE_MOVE && w->qhead != w->qtail) {
        struct gui_event *last = &w->queue[(w->qhead + WIN_QUEUE - 1) % WIN_QUEUE];
        if (last->type == GUI_EV_MOUSE_MOVE) {     /* siljishlar birlashtiriladi */
            last->x = x;
            last->y = y;
            last->buttons = buttons;
            return;
        }
    }
    int next = (w->qhead + 1) % WIN_QUEUE;
    if (next == w->qtail)
        w->qtail = (w->qtail + 1) % WIN_QUEUE;     /* to'la: eng eskisini tashlaymiz */
    struct gui_event *e = &w->queue[w->qhead];
    e->type = type;
    e->x = x;
    e->y = y;
    e->key = key;
    e->buttons = buttons;
    w->qhead = next;
}

static void close_window(window_t *w)
{
    if (w->owner) {
        /* Dasturga "yopil" deb xabar beramiz. Ikkinchi marta bosilsa — majburan to'xtatamiz
           (dastur osilib qolgan bo'lishi mumkin) */
        if (w->close_tick) {
            /* Signal yuboramiz va kutmaymiz (wm qulfi bizda — dastur shu qulfni kutayotgan
               bo'lishi mumkin). U tugagach oynasini collect_garbage yopadi */
            klog("wm: PID %u javob bermadi - to'xtatildi", w->owner);
            task_signal_kill(w->owner, EXIT_KILLED);
        } else {
            queue_push(w, GUI_EV_CLOSE, 0, 0, 0, 0);
            w->close_tick = timer_ticks() | 1;
        }
        return;
    }
    if (w->ops && w->ops->close)
        w->ops->close(w);
    else
        wm_destroy(w);
}

static window_t *window_at(int x, int y)
{
    for (int i = zcount - 1; i >= 0; i--)
        if (!zorder[i]->minimized && rect_contains(frame_rect(zorder[i]), x, y))
            return zorder[i];
    return 0;
}

/* ------------------------------ Kirishni qayta ishlash ------------------------------ */

static void deliver_mouse(window_t *w, int type)
{
    int lx = mx - (w->x + 1), ly = my - (w->y + TITLE_H);
    int buttons = mbuttons & 3;
    if (w->owner)
        queue_push(w, type, lx, ly, 0, buttons);
    else if (w->ops && w->ops->mouse)
        w->ops->mouse(w, type, lx, ly, buttons);
}

static void handle_key(char key)
{
    if (menu_open && key == KEY_ESC) {
        menu_open = false;
        invalidate_screen(menu_rect());
        return;
    }
    if (!focus || focus->minimized)
        return;
    if (focus->owner)
        queue_push(focus, GUI_EV_KEY, 0, 0, (uint8_t)key, 0);
    else if (focus->ops && focus->ops->key)
        focus->ops->key(focus, key);
}

static void cursor_invalidate(void)
{
    invalidate_screen(rect_make(mx, my, 12, 19));
}

static void on_move(void)
{
    if (drag_win) {
        invalidate_frame(drag_win);
        drag_win->x = mx - drag_dx;
        drag_win->y = my - drag_dy;
        rect_t f = frame_rect(drag_win);
        if (drag_win->y < 0) drag_win->y = 0;
        if (drag_win->y > SCREEN_H - TASKBAR_H - TITLE_H) drag_win->y = SCREEN_H - TASKBAR_H - TITLE_H;
        if (drag_win->x < 60 - f.w) drag_win->x = 60 - f.w;
        if (drag_win->x > SCREEN_W - 60) drag_win->x = SCREEN_W - 60;
        invalidate_frame(drag_win);
        return;
    }
    if (capture) {
        deliver_mouse(capture, GUI_EV_MOUSE_MOVE);
        return;
    }
    if (menu_open) {
        int hover = -1;
        for (int i = 0; i < MENU_COUNT; i++)
            if (menu_items[i][0] != '-' && rect_contains(menu_item_rect(i), mx, my))
                hover = i;
        if (hover != menu_hover) {
            menu_hover = hover;
            invalidate_screen(menu_rect());
        }
        return;
    }
    window_t *w = window_at(mx, my);
    if (w && w == focus && w->owner && rect_contains(content_rect(w), mx, my))
        deliver_mouse(w, GUI_EV_MOUSE_MOVE);       /* user dasturlar "hover" ni ham oladi */
}

static void on_press(int button)
{
    if (menu_open) {
        menu_open = false;
        invalidate_screen(menu_rect());
        if (rect_contains(menu_rect(), mx, my)) {
            for (int i = 0; i < MENU_COUNT; i++)
                if (menu_items[i][0] != '-' && rect_contains(menu_item_rect(i), mx, my))
                    menu_action(i);
            return;
        }
        if (rect_contains(start_button(), mx, my))
            return;                            /* Start tugmasi menyuni yopdi */
    }

    if (rect_contains(taskbar_rect(), mx, my)) {
        if (rect_contains(start_button(), mx, my)) {
            menu_open = true;
            menu_hover = -1;
            invalidate_screen(menu_rect());
            invalidate_screen(start_button());
            return;
        }
        int k = 0;
        for (int i = 0; i < MAX_WINDOWS; i++) {
            if (!windows[i].used)
                continue;
            if (rect_contains(task_button(k), mx, my)) {
                window_t *w = &windows[i];
                if (w->minimized || w != focus)
                    wm_focus(w);
                else
                    minimize(w);
                invalidate_screen(taskbar_rect());
                return;
            }
            k++;
        }
        return;
    }

    window_t *w = window_at(mx, my);
    if (w) {
        wm_focus(w);
        if (button != 1) {
            if (rect_contains(content_rect(w), mx, my)) {
                capture = w;
                deliver_mouse(w, GUI_EV_MOUSE_DOWN);
            }
            return;
        }
        if (rect_contains(close_button(w), mx, my)) {
            close_window(w);
            return;
        }
        if (rect_contains(min_button(w), mx, my)) {
            minimize(w);
            return;
        }
        if (my < w->y + TITLE_H) {             /* sarlavha: sudrab ko'chirish */
            drag_win = w;
            drag_dx = mx - w->x;
            drag_dy = my - w->y;
            return;
        }
        if (rect_contains(content_rect(w), mx, my)) {
            capture = w;
            deliver_mouse(w, GUI_EV_MOUSE_DOWN);
        }
        return;
    }

    /* Ish stoli: ikonka bir marta bosilsa — tanlanadi, ikki marta — ochiladi */
    int hit = -1;
    for (int i = 0; i < ICON_COUNT_DESKTOP; i++)
        if (rect_contains(icon_rect(i), mx, my))
            hit = i;
    uint32_t now = timer_ticks();
    if (hit >= 0 && hit == last_click_icon && now - last_click_tick < 45) {
        last_click_icon = -1;
        launch(hit);
    } else {
        last_click_icon = hit;
        last_click_tick = now;
    }
    if (hit != icon_selected) {
        if (icon_selected >= 0) invalidate_screen(icon_rect(icon_selected));
        if (hit >= 0) invalidate_screen(icon_rect(hit));
        icon_selected = hit;
    }
}

static void on_release(void)
{
    if (drag_win && !(mbuttons & 1))
        drag_win = 0;
    if (capture) {
        window_t *w = capture;
        if (!(mbuttons & 3))
            capture = 0;
        deliver_mouse(w, GUI_EV_MOUSE_UP);
    }
}

static void handle_mouse(const input_t *e)
{
    if (e->dx || e->dy) {
        cursor_invalidate();
        mx += e->dx;
        my += e->dy;
        if (mx < 0) mx = 0;
        if (my < 0) my = 0;
        if (mx > SCREEN_W - 1) mx = SCREEN_W - 1;
        if (my > SCREEN_H - 1) my = SCREEN_H - 1;
        cursor_invalidate();
        on_move();
    }
    if (e->dz) {
        window_t *w = window_at(mx, my);
        if (w && rect_contains(content_rect(w), mx, my)) {
            if (w->owner)
                queue_push(w, GUI_EV_WHEEL, mx - w->x - 1, my - w->y - TITLE_H, e->dz < 0 ? -1 : 1, 0);
            else if (w->ops && w->ops->wheel)
                w->ops->wheel(w, e->dz);
        }
    }
    uint8_t pressed = e->buttons & ~mbuttons;
    uint8_t released = mbuttons & ~e->buttons;
    mbuttons = e->buttons;
    if (pressed & 1) on_press(1);
    if (pressed & 2) on_press(2);
    if (released & 3) on_release();
}

/* ------------------------------- Chizish ------------------------------- */

static const char *cursor_shape[19] = {
    "X           ",
    "XX          ",
    "X.X         ",
    "X..X        ",
    "X...X       ",
    "X....X      ",
    "X.....X     ",
    "X......X    ",
    "X.......X   ",
    "X........X  ",
    "X.........X ",
    "X......XXXXX",
    "X...X..X    ",
    "X..XX..X    ",
    "X.X  X..X   ",
    "XX   X..X   ",
    "X     X..X  ",
    "      X..X  ",
    "       XX   ",
};

static void draw_cursor(surface_t *s)
{
    for (int y = 0; y < 19; y++)
        for (int x = 0; x < 12; x++) {
            char c = cursor_shape[y][x];
            if (c == 'X')
                gfx_pixel(s, mx + x, my + y, RGB(0, 0, 0));
            else if (c == '.')
                gfx_pixel(s, mx + x, my + y, RGB(255, 255, 255));
        }
}

static void text_shadow(surface_t *s, int x, int y, const char *t, uint32_t c)
{
    gfx_text(s, x + 1, y + 1, t, RGB(0, 0, 0), GFX_TRANSPARENT);
    gfx_text(s, x, y, t, c, GFX_TRANSPARENT);
}

static void draw_desktop(surface_t *s)
{
    gfx_gradient_v(s, 0, 0, SCREEN_W, SCREEN_H - TASKBAR_H, COL_DESKTOP_TOP, COL_DESKTOP_BOT);

    /* Markazdagi yozuv */
    const char *logo = OS_NAME;
    int scale = 8, lw = gfx_text_width(logo) * scale;
    int lx = (SCREEN_W - lw) / 2 + 40, ly = 230;
    gfx_text_scaled(s, lx + 4, ly + 4, logo, scale, RGB(14, 40, 66));
    gfx_text_scaled(s, lx, ly, logo, scale, RGB(52, 104, 150));
    const char *sub = "v" OS_VERSION "  |  C + Assembly, x86";
    gfx_text(s, (SCREEN_W - gfx_text_width(sub)) / 2 + 40, ly + 16 * scale + 10, sub,
             RGB(150, 190, 215), GFX_TRANSPARENT);

    for (int i = 0; i < ICON_COUNT_DESKTOP; i++) {
        rect_t r = icon_rect(i);
        if (i == icon_selected) {
            gfx_round_rect(s, r.x, r.y, r.w, r.h, RGB(60, 110, 170));
            gfx_rect(s, r.x + 1, r.y + 1, r.w - 2, r.h - 2, RGB(120, 170, 230));
        }
        ui_icon(s, r.x + (r.w - 32) / 2, r.y + 8, desktop_icons[i].icon);
        const char *label = desktop_icons[i].label;
        text_shadow(s, r.x + (r.w - gfx_text_width(label)) / 2, r.y + 48, label, RGB(255, 255, 255));
    }
}

static void draw_window(surface_t *s, const window_t *w)
{
    bool active = (w == focus);
    rect_t f = frame_rect(w);

    gfx_darken(s, f.x + 5, f.y + f.h, f.w, 5);                 /* soya: pastda va o'ngda */
    gfx_darken(s, f.x + f.w, f.y + 5, 5, f.h - 5);

    gfx_rect(s, f.x, f.y, f.w, f.h, COL_BORDER);
    gfx_gradient_v(s, f.x + 1, f.y + 1, f.w - 2, TITLE_H - 1,
                   active ? COL_TITLE_ACTIVE : COL_TITLE_IDLE,
                   active ? COL_TITLE_ACTIVE2 : COL_TITLE_IDLE2);

    char title[48];
    int max_chars = (f.w - 70) / FONT_W;
    if (max_chars < 0)
        max_chars = 0;
    strlcpy(title, w->title, (size_t)(max_chars + 1) < sizeof(title) ? (size_t)(max_chars + 1) : sizeof(title));
    gfx_text_bold(s, f.x + 10, f.y + 5, title, RGB(255, 255, 255));

    rect_t mb = min_button(w), cb = close_button(w);
    gfx_round_rect(s, mb.x, mb.y, mb.w, mb.h, active ? RGB(70, 125, 210) : RGB(130, 138, 150));
    gfx_hline(s, mb.x + 6, mb.y + 12, 9, RGB(255, 255, 255));
    gfx_hline(s, mb.x + 6, mb.y + 13, 9, RGB(255, 255, 255));
    gfx_round_rect(s, cb.x, cb.y, cb.w, cb.h, active ? RGB(214, 64, 64) : RGB(150, 120, 120));
    for (int i = 0; i < 8; i++) {                               /* "x" belgisi */
        gfx_fill(s, cb.x + 6 + i, cb.y + 5 + i, 2, 1, RGB(255, 255, 255));
        gfx_fill(s, cb.x + 13 - i, cb.y + 5 + i, 2, 1, RGB(255, 255, 255));
    }

    rect_t c = content_rect(w);
    gfx_blit(s, c.x, c.y, &w->surf, 0, 0, w->surf.w, w->surf.h);
}

static void draw_menu(surface_t *s)
{
    rect_t m = menu_rect();
    gfx_darken(s, m.x + 5, m.y + 5, m.w, m.h);
    gfx_fill(s, m.x, m.y, m.w, m.h, RGB(250, 251, 253));
    gfx_rect(s, m.x, m.y, m.w, m.h, COL_BORDER);
    gfx_gradient_v(s, m.x + 1, m.y + 1, m.w - 2, MENU_HEAD_H, COL_TITLE_ACTIVE, COL_TITLE_ACTIVE2);
    gfx_text_bold(s, m.x + 14, m.y + 6, OS_NAME " v" OS_VERSION, RGB(255, 255, 255));
    gfx_text(s, m.x + 14, m.y + 22, OS_AUTHOR, RGB(190, 210, 240), GFX_TRANSPARENT);
    for (int i = 0; i < MENU_COUNT; i++) {
        rect_t r = menu_item_rect(i);
        if (menu_items[i][0] == '-') {
            gfx_hline(s, r.x + 6, r.y + 4, r.w - 12, RGB(205, 210, 218));
            continue;
        }
        if (i == menu_hover)
            gfx_fill(s, r.x, r.y, r.w, r.h, COL_SELECT);
        gfx_text(s, r.x + 12, r.y + 5, menu_items[i], i >= 8 ? RGB(150, 40, 40) : COL_TEXT, GFX_TRANSPARENT);
    }
}

static void draw_taskbar(surface_t *s)
{
    rect_t t = taskbar_rect();
    gfx_gradient_v(s, t.x, t.y, t.w, t.h, RGB(40, 44, 54), RGB(18, 20, 26));
    gfx_hline(s, 0, t.y, SCREEN_W, RGB(80, 90, 110));

    rect_t sb = start_button();
    gfx_round_rect(s, sb.x, sb.y, sb.w, sb.h, menu_open ? RGB(70, 130, 220) : COL_TITLE_ACTIVE);
    ui_logo(s, sb.x + 6, sb.y + 3, 20);
    gfx_text_bold(s, sb.x + 34, sb.y + 5, OS_NAME, RGB(255, 255, 255));

    int k = 0;
    for (int i = 0; i < MAX_WINDOWS; i++) {
        const window_t *w = &windows[i];
        if (!w->used)
            continue;
        rect_t r = task_button(k++);
        if (r.x + r.w > SCREEN_W - 190)
            break;                                 /* soat joyiga chiqib ketmasin */
        bool active = (w == focus && !w->minimized);
        gfx_round_rect(s, r.x, r.y, r.w, r.h, active ? RGB(78, 90, 116) : RGB(48, 54, 66));
        if (active)
            gfx_hline(s, r.x + 4, r.y + r.h - 2, r.w - 8, RGB(110, 170, 250));
        char label[18];
        strlcpy(label, w->title, sizeof(label));
        gfx_text(s, r.x + 10, r.y + 5, label, w->minimized ? RGB(140, 148, 160) : RGB(235, 238, 242),
                 GFX_TRANSPARENT);
    }

    /* O'ng tomon: protsessor bandligi va soat */
    uint32_t cpu = 0;
    for (int i = 0; i < MAX_TASKS; i++) {
        task_t *tk = task_get(i);
        if (tk->state != TASK_UNUSED && tk->pid == 0)
            cpu = 100 - (tk->cpu_percent > 100 ? 100 : tk->cpu_percent);
    }
    char cpu_text[16];
    ksnprintf(cpu_text, sizeof(cpu_text), "CPU %u%%", cpu);
    gfx_text(s, SCREEN_W - 170, t.y + 9, cpu_text, RGB(150, 160, 175), GFX_TRANSPARENT);
    gfx_text_bold(s, SCREEN_W - 84, t.y + 9, clock_text, RGB(255, 255, 255));
}

/* Iflos hududni qayta yig'ib, framebuffer ga ko'chirish */
static void compose(void)
{
    rect_t d;
    if (!rect_intersect(dirty, rect_make(0, 0, SCREEN_W, SCREEN_H), &d)) {
        dirty = rect_make(0, 0, 0, 0);
        return;
    }
    dirty = rect_make(0, 0, 0, 0);
    surface_set_clip(&back, d);

    draw_desktop(&back);
    for (int i = 0; i < zcount; i++) {
        rect_t tmp;
        if (!zorder[i]->minimized && rect_intersect(frame_with_shadow(zorder[i]), d, &tmp))
            draw_window(&back, zorder[i]);
    }
    if (menu_open)
        draw_menu(&back);
    rect_t tmp;
    if (rect_intersect(taskbar_rect(), d, &tmp))
        draw_taskbar(&back);
    draw_cursor(&back);

    for (int row = d.y; row < d.y + d.h; row++)
        memcpy(&screen.px[row * screen.stride + d.x], &back.px[row * back.stride + d.x], (size_t)d.w * 4);
}

/* ------------------------------ Asosiy tsikl ------------------------------ */

/* Egasi tugagan user oynalarni yopish, kuzatilgan dasturlarni tozalash */
static void collect_garbage(void)
{
    for (int i = 0; i < MAX_WINDOWS; i++) {
        window_t *w = &windows[i];
        if (!w->used || !w->owner)
            continue;
        task_t *t = task_find(w->owner);
        if (!t || t->state == TASK_ZOMBIE)
            wm_destroy(w);
    }
    for (int i = 0; i < 8; i++) {
        if (!tracked[i])
            continue;
        task_t *t = task_find(tracked[i]);
        if (!t) {
            tracked[i] = 0;
        } else if (t->state == TASK_ZOMBIE) {
            task_reap_pid(tracked[i]);
            tracked[i] = 0;
        }
    }
}

static void teardown(void)
{
    mouse_set_hook(0);
    keyboard_set_hook(0);
    wm_task = 0;
    running = false;
    focus_terminal = false;
    for (int i = 0; i < MAX_WINDOWS; i++) {
        if (!windows[i].used)
            continue;
        if (windows[i].owner)
            task_signal_kill(windows[i].owner, EXIT_KILLED);   /* kutmasdan: yuqoridagi izoh */
        wm_destroy(&windows[i]);
    }
    fb_text_mode();
    vga_redirect(false);
    surface_free(&back);
    klog("gui: to'xtatildi");
}

static void wm_thread(uint32_t arg)
{
    (void)arg;
    for (;;) {
        wm_lock();
        if (stop_requested) {
            teardown();
            wm_unlock();
            task_exit();
        }

        input_t e;
        int budget = 64;                       /* bir aylanishda juda ko'p hodisa bo'lsa ham ekran yangilansin */
        while (budget-- > 0 && input_pop(&e)) {
            if (e.kind == IN_KEY)
                handle_key(e.key);
            else
                handle_mouse(&e);
        }

        uint32_t sec = timer_seconds();
        if (sec != last_second) {
            last_second = sec;
            rtc_time_t t;
            rtc_read(&t);
            ksnprintf(clock_text, sizeof(clock_text), "%02u:%02u:%02u", t.hour, t.minute, t.second);
            invalidate_screen(taskbar_rect());
            for (int i = 0; i < MAX_WINDOWS; i++)
                if (windows[i].used && windows[i].ops && windows[i].ops->tick)
                    windows[i].ops->tick(&windows[i]);
        }
        for (int i = 0; i < MAX_WINDOWS; i++)
            if (windows[i].used && windows[i].ops && windows[i].ops->frame)
                windows[i].ops->frame(&windows[i]);

        collect_garbage();
        compose();
        wm_unlock();
        /* Keyingi tikgacha uxlaymiz (terminal, soat, animatsiyalar uchun ~100 marta
           soniyada). Sichqoncha yoki klaviatura bundan oldin uyg'otishi mumkin */
        task_sleep_ticks(1);
    }
}

bool gui_available(void)
{
    return fb_info()->available;
}

bool gui_active(void)
{
    return running;
}

const char *gui_start(void)
{
    if (running)
        return "grafik rejim allaqachon ishlayapti";
    if (!fb_info()->available)
        return fb_info()->boot ? "ekran formati grafik interfeysga mos emas (32 bit RGB kerak)"
                               : "grafik adapter topilmadi (QEMU/UTM da 'VGA' videokarta kerak)";
    if (!surface_init(&back, SCREEN_W, SCREEN_H))
        return "orqa bufer uchun xotira yetmadi (3 MB)";
    /* Avval matnni xotiraga ko'chiramiz: grafik rejim yoqilgach 0xB8000 dagi matn yo'qoladi */
    vga_redirect(true);
    if (!fb_set_mode(SCREEN_W, SCREEN_H)) {
        vga_redirect(false);
        surface_free(&back);
        return fb_info()->boot ? "ekran 1024x768 dan kichik" : "videokarta 1024x768 rejimini qabul qilmadi";
    }
    const fb_info_t *fi = fb_info();
    surface_wrap(&screen, fi->pixels, (int)fi->width, (int)fi->height, (int)(fi->pitch / 4));
    if (fi->width > SCREEN_W || fi->height > SCREEN_H)    /* katta ekran: chetlari qora */
        gfx_fill(&screen, 0, 0, (int)fi->width, (int)fi->height, 0);

    memset(windows, 0, sizeof(windows));
    memset(tracked, 0, sizeof(tracked));
    zcount = 0;
    focus = 0;
    capture = drag_win = 0;
    menu_open = false;
    icon_selected = last_click_icon = -1;
    cascade = 0;
    mx = SCREEN_W / 2;
    my = SCREEN_H / 2;
    mbuttons = 0;
    rem_x = rem_y = 0;
    in_head = in_tail = 0;
    last_second = 0;
    stop_requested = false;
    dirty = rect_make(0, 0, SCREEN_W, SCREEN_H);

    running = true;
    app_terminal_open();
    mouse_set_hook(mouse_hook);
    keyboard_set_hook(key_hook);

    uint32_t pid = task_create("wm", wm_thread, 0);
    if (!pid) {
        stop_requested = true;
        teardown();
        return "oyna menejeri vazifasini yaratib bo'lmadi";
    }
    wm_task = task_find(pid);
    klog("gui: ishga tushdi %ux%u", SCREEN_W, SCREEN_H);
    return 0;
}

void gui_request_stop(void)
{
    stop_requested = true;
}

/* Oyna menejeri oqimining PID i (0 — grafik rejim yo'q). "kill" uni to'g'ridan-to'g'ri
   o'ldirmasligi uchun: ekran grafik rejimda egasiz qolib ketardi */
uint32_t gui_wm_pid(void)
{
    return (running && wm_task) ? wm_task->pid : 0;
}

/* ------------------------------ Syscall lar ------------------------------ */

static window_t *user_window(int id)
{
    uint32_t pid = task_current()->pid;
    for (int i = 0; i < MAX_WINDOWS; i++)
        if (windows[i].used && windows[i].id == id && windows[i].owner == pid)
            return &windows[i];
    return 0;
}

int gui_sys_create(const char *title, int w, int h)
{
    if (!running)
        return -19;                            /* ENODEV: grafik rejim yo'q */
    if (w < 16 || h < 16 || w > 800 || h > 600)
        return -22;                            /* EINVAL */
    wm_lock();
    int count = 0;
    uint32_t pid = task_current()->pid;
    for (int i = 0; i < MAX_WINDOWS; i++)
        if (windows[i].used && windows[i].owner == pid)
            count++;
    int ret = -24;                             /* EMFILE: bitta dasturga 4 tadan ko'p oyna yo'q */
    if (count < 4) {
        window_t *win = wm_create(title, w, h, 0, 0);
        if (win) {
            win->owner = pid;
            ret = win->id;
        } else {
            ret = -12;                         /* ENOMEM yoki oynalar jadvali to'la */
        }
    }
    wm_unlock();
    return ret;
}

bool gui_sys_window_size(int id, int *w, int *h)
{
    if (!running)
        return false;
    wm_lock();
    window_t *win = user_window(id);
    if (win) {
        *w = win->surf.w;
        *h = win->surf.h;
    }
    wm_unlock();
    return win != 0;
}

int gui_sys_update(int id, const uint32_t *pixels)
{
    wm_lock();
    window_t *win = running ? user_window(id) : 0;
    if (win) {
        memcpy(win->surf.px, pixels, (size_t)win->surf.w * (size_t)win->surf.h * 4);
        wm_invalidate(win);
    }
    wm_unlock();
    return win ? 0 : -2;
}

int gui_sys_event(int id, struct gui_event *ev)
{
    wm_lock();
    window_t *win = running ? user_window(id) : 0;
    int ret = -2;                              /* oyna yo'q: dastur tugashi kerak */
    if (win) {
        ret = 0;
        if (win->qhead != win->qtail) {
            *ev = win->queue[win->qtail];
            win->qtail = (win->qtail + 1) % WIN_QUEUE;
            ret = 1;
        }
    }
    wm_unlock();
    return ret;
}

int gui_sys_close(int id)
{
    wm_lock();
    window_t *win = running ? user_window(id) : 0;
    if (win)
        wm_destroy(win);
    wm_unlock();
    return win ? 0 : -2;
}

/* v0.10: exec — eski dasturning oynalari yangi dasturga meros qolmaydi */
void gui_sys_close_all(void)
{
    wm_lock();
    uint32_t pid = task_current()->pid;
    for (int i = 0; running && i < MAX_WINDOWS; i++)
        if (windows[i].used && windows[i].owner == pid)
            wm_destroy(&windows[i]);
    wm_unlock();
}
