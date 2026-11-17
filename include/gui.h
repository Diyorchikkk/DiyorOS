/*
 * gui.h — oyna menejeri va grafik dasturlar (v0.7).
 */
#ifndef GUI_H
#define GUI_H

#include <stdint.h>
#include <stdbool.h>
#include "gfx.h"
#include "syscall_nums.h"

#define SCREEN_W   1024
#define SCREEN_H   768
#define TASKBAR_H  34
#define TITLE_H    26
#define MAX_WINDOWS 16
#define WIN_QUEUE  64

/* Ranglar (umumiy uslub) */
#define COL_DESKTOP_TOP   RGB(24, 44, 78)
#define COL_DESKTOP_BOT   RGB(14, 104, 120)
#define COL_TITLE_ACTIVE  RGB(40, 98, 190)
#define COL_TITLE_ACTIVE2 RGB(28, 70, 150)
#define COL_TITLE_IDLE    RGB(110, 118, 130)
#define COL_TITLE_IDLE2   RGB(90, 96, 108)
#define COL_WINDOW_BG     RGB(242, 244, 247)
#define COL_BORDER        RGB(40, 46, 56)
#define COL_TEXT          RGB(28, 32, 40)
#define COL_TEXT_DIM      RGB(110, 118, 130)
#define COL_ACCENT        RGB(40, 98, 190)
#define COL_SELECT        RGB(200, 220, 250)
#define COL_BUTTON        RGB(225, 229, 235)

typedef struct window window_t;

/* Kernel ichidagi grafik dasturlar shu funksiyalar orqali hodisa oladi.
   Hammasi oyna menejeri oqimida (wm), qulf ushlangan holda chaqiriladi */
typedef struct {
    void (*mouse)(window_t *w, int type, int x, int y, int buttons);
    void (*key)(window_t *w, char key);
    void (*wheel)(window_t *w, int dz);
    void (*tick)(window_t *w);          /* har soniyada */
    void (*frame)(window_t *w);         /* har aylanishda (~100 marta / s) */
    void (*close)(window_t *w);         /* [x] bosildi; 0 bo'lsa — oyna yopiladi */
    void (*destroy)(window_t *w);       /* oyna yo'q qilinmoqda: dastur ma'lumotini bo'shatish */
} window_ops_t;

struct window {
    bool              used;
    int               id;
    char              title[40];
    int               x, y;             /* ramkaning chap yuqori burchagi (ekranda) */
    surface_t         surf;             /* oyna ichidagi tasvir */
    bool              minimized;
    uint32_t          owner;            /* 0 — kernel dasturi, aks holda — jarayon PID */
    const window_ops_t *ops;
    void             *data;
    uint32_t          close_tick;       /* user oyna: [x] birinchi marta bosilgan vaqt */
    struct gui_event  queue[WIN_QUEUE]; /* user oynalar uchun hodisalar navbati */
    int               qhead, qtail;
};

/* Ishga tushirish / to'xtatish */
bool        gui_available(void);
bool        gui_active(void);
const char *gui_start(void);          /* 0 — muvaffaqiyat, aks holda xato matni */
void        gui_request_stop(void);
uint32_t    gui_wm_pid(void);         /* oyna menejeri oqimi (0 — grafik rejim yo'q) */
void        gui_set_mouse_speed(int speed);   /* 1..10, 5 — standart */
int         gui_mouse_speed(void);

/* Oynalar (qulf ushlangan holda — kernel dasturlari ichidan) */
window_t   *wm_create(const char *title, int w, int h, const window_ops_t *ops, void *data);
void        wm_destroy(window_t *w);
void        wm_invalidate(window_t *w);
void        wm_invalidate_rect(window_t *w, rect_t r);   /* oyna ichidagi koordinatalarda */
void        wm_focus(window_t *w);
void        wm_set_title(window_t *w, const char *title);
uint32_t    wm_ticks(void);
void        wm_track_process(uint32_t pid);   /* oyna menejeri ishga tushirgan dastur: tugaganda tozalanadi */

/* Kernel grafik dasturlari */
void app_terminal_open(void);
void app_files_open(const char *path);
void app_monitor_open(void);
void app_paint_open(void);
void app_about_open(void);
void app_viewer_open(const char *path);
bool app_is_terminal(const window_t *w);
void app_run_program(const char *path);

/* Grafik vidjetlar (dasturlar uchun umumiy) */
void ui_button(surface_t *s, int x, int y, int w, int h, const char *label, bool pressed);
void ui_icon(surface_t *s, int x, int y, int kind);   /* 32x32 */
void ui_logo(surface_t *s, int x, int y, int size);   /* DiyorOS logotipi */
enum { ICON_TERMINAL, ICON_FILES, ICON_MONITOR, ICON_PAINT, ICON_MANDEL, ICON_SNAKE,
       ICON_ABOUT, ICON_FOLDER, ICON_FILE, ICON_PROGRAM, ICON_IMAGE, ICON_COUNT };

/* Syscall lar (syscall.c chaqiradi; ko'rsatkichlar tekshirilgan) */
int  gui_sys_create(const char *title, int w, int h);
int  gui_sys_update(int id, const uint32_t *pixels);
int  gui_sys_event(int id, struct gui_event *ev);
int  gui_sys_close(int id);
void gui_sys_close_all(void);              /* v0.10: joriy jarayonning barcha oynalari (exec) */
bool gui_sys_window_size(int id, int *w, int *h);

#endif
