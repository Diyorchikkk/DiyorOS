/*
 * viewer.c — fayl ko'ruvchi: matn va BMP rasmlar (v0.7).
 *
 * Matn: fayl xotiraga o'qiladi (64 KB gacha), qatorlar boshlanish joyi
 * jadvalga yoziladi — varaqlashda faqat ko'rinadigan qatorlar chiziladi.
 * Rasm: BMP fayl piksellarga aylantiriladi (gfx_bmp_decode). Rasm oynadan
 * katta bo'lsa — butun songa bo'linib kichraytiriladi.
 */
#include "gui.h"
#include "vfs.h"
#include "string.h"
#include "memory.h"
#include "kprintf.h"
#include "drivers.h"

#define TEXT_MAX   (64 * 1024)
#define IMAGE_MAX  VFS_FILE_MAX
#define MAX_LINES  4096
#define COLS       80
#define ROWS       24
#define STATUS_H   20

typedef struct {
    bool      image;
    char     *text;
    uint32_t  len;
    uint32_t *lines;      /* har qatorning boshlanish joyi */
    int       line_count;
    int       scroll;
    bool      truncated;
} viewer_t;

/* Faylni to'liq o'qish. Natija kmalloc da, chaqiruvchi bo'shatadi */
static uint8_t *read_file(const char *path, uint32_t max, uint32_t *out_len, bool *truncated)
{
    vfs_node_t *n;
    if (vfs_lookup(path, &n) != VFS_OK || n->type != VFS_FILE)
        return 0;
    uint32_t size = n->size;
    *truncated = size > max;
    if (size > max)
        size = max;
    uint8_t *buf = kmalloc(size + 1);
    if (!buf)
        return 0;
    int fd = vfs_open(path, VFS_O_READ);
    if (fd < 0) {
        kfree(buf);
        return 0;
    }
    uint32_t got = 0;
    int32_t r;
    while (got < size && (r = vfs_read(fd, buf + got, size - got)) > 0)
        got += (uint32_t)r;
    vfs_close(fd);
    buf[got] = 0;
    *out_len = got;
    return buf;
}

static void draw_text(window_t *w)
{
    viewer_t *v = w->data;
    surface_t *s = &w->surf;
    gfx_fill(s, 0, 0, s->w, s->h, RGB(255, 255, 255));
    for (int row = 0; row < ROWS; row++) {
        int li = v->scroll + row;
        if (li >= v->line_count)
            break;
        uint32_t p = v->lines[li];
        for (int col = 0; col < COLS && p < v->len && v->text[p] != '\n'; p++) {
            char c = v->text[p];
            if (c == '\t') {
                col = (col + 4) & ~3;
                continue;
            }
            if ((uint8_t)c >= 32)
                gfx_char(s, 4 + col * FONT_W, 2 + row * FONT_H, c, COL_TEXT, GFX_TRANSPARENT);
            col++;
        }
    }
    gfx_fill(s, 0, s->h - STATUS_H, s->w, STATUS_H, RGB(236, 239, 243));
    char status[96];
    ksnprintf(status, sizeof(status), "Qator %d-%d / %d%s   (g'ildirak, strelkalar, PgUp/PgDn)",
              v->line_count ? v->scroll + 1 : 0,
              v->scroll + ROWS < v->line_count ? v->scroll + ROWS : v->line_count,
              v->line_count, v->truncated ? "  [faqat 64 KB]" : "");
    gfx_text(s, 6, s->h - STATUS_H + 2, status, COL_TEXT_DIM, GFX_TRANSPARENT);
    wm_invalidate(w);
}

static void scroll_by(window_t *w, int delta)
{
    viewer_t *v = w->data;
    if (v->image)
        return;
    v->scroll += delta;
    if (v->scroll > v->line_count - ROWS)
        v->scroll = v->line_count - ROWS;
    if (v->scroll < 0)
        v->scroll = 0;
    draw_text(w);
}

static void viewer_wheel(window_t *w, int dz)
{
    scroll_by(w, dz < 0 ? -3 : 3);
}

static void viewer_key(window_t *w, char key)
{
    switch (key) {
    case KEY_UP:   scroll_by(w, -1); break;
    case KEY_DOWN: scroll_by(w, 1); break;
    case KEY_PGUP: scroll_by(w, -ROWS); break;
    case KEY_PGDN: scroll_by(w, ROWS); break;
    case KEY_HOME: scroll_by(w, -MAX_LINES); break;
    case KEY_END:  scroll_by(w, MAX_LINES); break;
    }
}

static void viewer_destroy(window_t *w)
{
    viewer_t *v = w->data;
    if (v->text)
        kfree(v->text);
    if (v->lines)
        kfree(v->lines);
    kfree(v);
}

static const window_ops_t viewer_ops = {
    .key = viewer_key,
    .wheel = viewer_wheel,
    .destroy = viewer_destroy,
};

static void open_image(const char *path, const uint8_t *data, uint32_t len)
{
    surface_t img;
    if (!gfx_bmp_decode(data, len, &img)) {
        kprintf("grafik: %s - BMP o'qib bo'lmadi (faqat 24/32 bit, siqilmagan)\n", path);
        return;
    }
    /* Ekranga sig'maydigan rasm: 2, 3... marta kichraytiramiz */
    int scale = 1;
    while (img.w / scale > 800 || img.h / scale > 560)
        scale++;
    int ww = img.w / scale, wh = img.h / scale;
    viewer_t *v = kmalloc(sizeof(viewer_t));
    if (!v) {
        surface_free(&img);
        return;
    }
    memset(v, 0, sizeof(*v));
    v->image = true;
    char title[40];
    ksnprintf(title, sizeof(title), "%s (%dx%d)", vfs_basename(path), img.w, img.h);
    window_t *w = wm_create(title, ww, wh, &viewer_ops, v);
    if (w) {
        for (int y = 0; y < wh; y++)
            for (int x = 0; x < ww; x++)
                w->surf.px[y * ww + x] = img.px[(y * scale) * img.w + x * scale];
        wm_invalidate(w);
    } else {
        kfree(v);
    }
    surface_free(&img);
}

void app_viewer_open(const char *path)
{
    uint32_t len = 0;
    bool truncated = false;
    size_t plen = strlen(path);
    bool bmp = plen > 4 && (strcmp(path + plen - 4, ".bmp") == 0 || strcmp(path + plen - 4, ".BMP") == 0);

    uint8_t *data = read_file(path, bmp ? IMAGE_MAX : TEXT_MAX, &len, &truncated);
    if (!data) {
        kprintf("grafik: %s ni ochib bo'lmadi\n", path);
        return;
    }
    if (bmp) {
        open_image(path, data, len);
        kfree(data);
        return;
    }

    viewer_t *v = kmalloc(sizeof(viewer_t));
    uint32_t *lines = kmalloc(sizeof(uint32_t) * MAX_LINES);
    if (!v || !lines) {
        if (v) kfree(v);
        if (lines) kfree(lines);
        kfree(data);
        return;
    }
    memset(v, 0, sizeof(*v));
    v->text = (char *)data;
    v->len = len;
    v->lines = lines;
    v->truncated = truncated;
    v->lines[v->line_count++] = 0;
    for (uint32_t i = 0; i < len && v->line_count < MAX_LINES; i++)
        if (data[i] == '\n' && i + 1 < len)
            v->lines[v->line_count++] = i + 1;

    window_t *w = wm_create(vfs_basename(path), COLS * FONT_W + 8, ROWS * FONT_H + 4 + STATUS_H, &viewer_ops, v);
    if (!w) {
        viewer_destroy(&(window_t){ .data = v });
        return;
    }
    draw_text(w);
}
