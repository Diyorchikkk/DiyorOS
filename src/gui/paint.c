/*
 * paint.c — "Chizish" dasturi (v0.7).
 *
 * Sichqoncha chap tugmasi — tanlangan rang bilan chizish, o'ng tugmasi —
 * o'chirg'ich. Sichqoncha tez yurganda paketlar orasida bir necha piksel
 * "sakrash" bo'ladi, shuning uchun nuqtalar emas, oldingi va hozirgi joy
 * orasida qalin chiziq chiziladi (gfx_thick_line).
 *
 * "Saqlash" rasmni BMP formatida diskka yozadi (/disk/rasm1.bmp ...).
 * BMP ni Mac, Windows, brauzer — hamma ocha oladi: DiyorOS da chizilgan rasm
 * disk.img ichidan haqiqiy kompyuterda ko'rinadi.
 */
#include "gui.h"
#include "vfs.h"
#include "string.h"
#include "memory.h"
#include "kprintf.h"

#define W 600
#define H 450
#define TOOLBAR_H 40
#define STATUS_H  22
#define CANVAS_Y  TOOLBAR_H
#define CANVAS_H  (H - TOOLBAR_H - STATUS_H)

static const uint32_t colors[12] = {
    RGB(20, 20, 24),   RGB(120, 124, 132), RGB(255, 255, 255), RGB(220, 50, 50),
    RGB(245, 140, 40), RGB(245, 205, 40),  RGB(60, 170, 70),   RGB(40, 170, 170),
    RGB(50, 110, 220), RGB(130, 70, 200),  RGB(230, 100, 170), RGB(130, 85, 50),
};
static const int sizes[3] = { 2, 5, 10 };

typedef struct {
    surface_t canvas;
    int       color;
    int       size;
    bool      drawing;
    int       last_x, last_y;
    char      status[96];
} paint_t;

static void draw_toolbar(window_t *w)
{
    paint_t *p = w->data;
    surface_t *s = &w->surf;
    gfx_gradient_v(s, 0, 0, W, TOOLBAR_H, RGB(246, 248, 251), RGB(226, 230, 236));
    gfx_hline(s, 0, TOOLBAR_H - 1, W, RGB(196, 202, 212));
    for (int i = 0; i < 12; i++) {
        int x = 8 + i * 24;
        if (i == p->color)
            gfx_fill(s, x - 2, 7, 24, 26, RGB(40, 98, 190));
        gfx_fill(s, x, 9, 20, 22, RGB(120, 128, 140));
        gfx_fill(s, x + 1, 10, 18, 20, colors[i]);
    }
    for (int i = 0; i < 3; i++) {
        int x = 304 + i * 30;
        gfx_round_rect(s, x, 7, 26, 26, i == p->size ? COL_SELECT : RGB(255, 255, 255));
        gfx_circle_fill(s, x + 13, 20, sizes[i] / 2 + 1, RGB(30, 34, 40));
    }
    ui_button(s, 404, 8, 86, 24, "Tozalash", false);
    ui_button(s, 498, 8, 92, 24, "Saqlash", false);
}

static void draw_status(window_t *w)
{
    paint_t *p = w->data;
    surface_t *s = &w->surf;
    gfx_fill(s, 0, H - STATUS_H, W, STATUS_H, RGB(236, 239, 243));
    gfx_hline(s, 0, H - STATUS_H, W, RGB(206, 212, 220));
    gfx_text(s, 8, H - STATUS_H + 3, p->status, COL_TEXT_DIM, GFX_TRANSPARENT);
    wm_invalidate_rect(w, rect_make(0, H - STATUS_H, W, STATUS_H));
}

/* Tuvalning bir qismini oyna sirtiga ko'chirib, ekranda yangilash */
static void show_canvas(window_t *w, rect_t r)
{
    paint_t *p = w->data;
    rect_t c;
    if (!rect_intersect(r, rect_make(0, 0, W, CANVAS_H), &c))
        return;
    gfx_blit(&w->surf, c.x, CANVAS_Y + c.y, &p->canvas, c.x, c.y, c.w, c.h);
    wm_invalidate_rect(w, rect_make(c.x, CANVAS_Y + c.y, c.w, c.h));
}

static void redraw_all(window_t *w)
{
    draw_toolbar(w);
    show_canvas(w, rect_make(0, 0, W, CANVAS_H));
    draw_status(w);
    wm_invalidate(w);
}

static void stroke(window_t *w, int x0, int y0, int x1, int y1, uint32_t color)
{
    paint_t *p = w->data;
    int r = sizes[p->size];
    gfx_thick_line(&p->canvas, x0, y0, x1, y1, r, color);
    int left = (x0 < x1 ? x0 : x1) - r - 1, top = (y0 < y1 ? y0 : y1) - r - 1;
    int right = (x0 > x1 ? x0 : x1) + r + 1, bottom = (y0 > y1 ? y0 : y1) + r + 1;
    show_canvas(w, rect_make(left, top, right - left + 1, bottom - top + 1));
}

static void save(window_t *w)
{
    paint_t *p = w->data;
    vfs_node_t *disk;
    /* vfs_lookup ulash nuqtasidan o'tib, diskning ildiz papkasini qaytaradi. Disk
       ulanmagan bo'lsa — bo'sh "mountpoint" papkaning o'zi qaytadi */
    bool have_disk = vfs_lookup("/disk", &disk) == VFS_OK && !(disk->mountpoint && !disk->mount);
    const char *dir = have_disk ? "/disk" : "/home/diyorbek";

    char path[VFS_PATH_MAX];
    vfs_node_t *tmp;
    int n;
    for (n = 1; n < 100; n++) {                  /* birinchi bo'sh nom: rasm1.bmp, rasm2.bmp ... */
        ksnprintf(path, sizeof(path), "%s/rasm%d.bmp", dir, n);
        if (vfs_lookup(path, &tmp) != VFS_OK)
            break;
    }
    uint32_t size = gfx_bmp_size(W, CANVAS_H);
    uint8_t *bmp = kmalloc(size);
    if (!bmp) {
        strcpy(p->status, "Xato: xotira yetmadi");
        draw_status(w);
        return;
    }
    gfx_bmp_encode(&p->canvas, bmp);
    int fd = vfs_open(path, VFS_O_WRITE | VFS_O_CREATE | VFS_O_TRUNC);
    int32_t written = fd >= 0 ? vfs_write(fd, bmp, size) : fd;
    if (fd >= 0)
        vfs_close(fd);
    kfree(bmp);
    if (written == (int32_t)size)
        ksnprintf(p->status, sizeof(p->status), "Saqlandi: %s (%u KB)%s", path, size / 1024,
                  have_disk ? "" : " - disk yo'q, xotirada");
    else
        ksnprintf(p->status, sizeof(p->status), "Xato: %s", vfs_strerror(written < 0 ? written : VFS_ENOSPC));
    draw_status(w);
}

static void paint_mouse(window_t *w, int type, int x, int y, int buttons)
{
    paint_t *p = w->data;
    int cy = y - CANVAS_Y;
    uint32_t color = (buttons & 2) ? RGB(255, 255, 255) : colors[p->color];   /* o'ng tugma — o'chirg'ich */

    if (type == GUI_EV_MOUSE_DOWN) {
        if (y < TOOLBAR_H) {
            for (int i = 0; i < 12; i++)
                if (x >= 8 + i * 24 && x < 28 + i * 24)
                    p->color = i;
            for (int i = 0; i < 3; i++)
                if (x >= 304 + i * 30 && x < 330 + i * 30)
                    p->size = i;
            if (x >= 404 && x < 490) {
                gfx_fill(&p->canvas, 0, 0, W, CANVAS_H, RGB(255, 255, 255));
                strcpy(p->status, "Tuval tozalandi");
                redraw_all(w);
                return;
            }
            if (x >= 498 && x < 590) {
                strcpy(p->status, "Saqlanmoqda...");
                save(w);
            }
            draw_toolbar(w);
            wm_invalidate_rect(w, rect_make(0, 0, W, TOOLBAR_H));
            return;
        }
        if (cy >= 0 && cy < CANVAS_H) {
            p->drawing = true;
            p->last_x = x;
            p->last_y = cy;
            stroke(w, x, cy, x, cy, color);
        }
        return;
    }
    if (type == GUI_EV_MOUSE_MOVE && p->drawing && (buttons & 3)) {
        stroke(w, p->last_x, p->last_y, x, cy, color);
        p->last_x = x;
        p->last_y = cy;
        return;
    }
    if (type == GUI_EV_MOUSE_UP)
        p->drawing = false;
}

static void paint_destroy(window_t *w)
{
    paint_t *p = w->data;
    surface_free(&p->canvas);
    kfree(p);
}

static const window_ops_t paint_ops = {
    .mouse = paint_mouse,
    .destroy = paint_destroy,
};

void app_paint_open(void)
{
    paint_t *p = kmalloc(sizeof(paint_t));
    if (!p)
        return;
    memset(p, 0, sizeof(*p));
    if (!surface_init(&p->canvas, W, CANVAS_H)) {
        kfree(p);
        return;
    }
    gfx_fill(&p->canvas, 0, 0, W, CANVAS_H, RGB(255, 255, 255));
    p->color = 8;
    p->size = 1;
    strcpy(p->status, "Chap tugma - chizish, o'ng tugma - o'chirish. Saqlash: BMP fayl");
    window_t *w = wm_create("Chizish", W, H, &paint_ops, p);
    if (!w) {
        paint_destroy(&(window_t){ .data = p });
        return;
    }
    redraw_all(w);
}
