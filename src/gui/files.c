/*
 * files.c — "Fayllar" dasturi: grafik fayl menejeri (v0.7).
 *
 * Shell dagi ls / cd ning grafik ko'rinishi. Barcha ma'lumot VFS orqali
 * olinadi, shuning uchun ramfs (/home), procfs (/proc) va diskdagi FAT32
 * (/disk) bir xil ko'rinadi — dastur qaysi fayl tizimi ekanini bilmaydi.
 *
 *   ikki marta bosish: papka — ichiga kirish, /bin dagi fayl — dasturni
 *   ishga tushirish, .bmp — rasm ko'ruvchi, boshqasi — matn ko'ruvchi
 *   strelkalar, Enter, Backspace — klaviatura bilan boshqarish
 */
#include "gui.h"
#include "vfs.h"
#include "string.h"
#include "memory.h"
#include "kprintf.h"
#include "drivers.h"

#define W 560
#define H 400
#define TOOLBAR_H 36
#define HEADER_Y  38
#define LIST_Y    58
#define ROW_H     20
#define STATUS_H  22
#define MAX_ENTRIES 256
#define VISIBLE_ROWS ((H - STATUS_H - LIST_Y) / ROW_H)

enum { K_PARENT, K_DIR, K_FILE, K_PROGRAM, K_IMAGE };

typedef struct {
    char     name[VFS_NAME_MAX];
    uint8_t  kind;
    uint32_t size;
} fentry_t;

typedef struct {
    char      path[VFS_PATH_MAX];
    fentry_t *entries;
    int       count;
    int       scroll;
    int       selected;
    int       last_index;
    uint32_t  last_click;
    char      status[96];
} files_t;

static bool ends_with(const char *s, const char *suffix)
{
    size_t a = strlen(s), b = strlen(suffix);
    if (b > a)
        return false;
    for (size_t i = 0; i < b; i++) {
        char c = s[a - b + i];
        if (c >= 'A' && c <= 'Z')
            c = (char)(c + 32);
        if (c != suffix[i])
            return false;
    }
    return true;
}

static void join(char *out, const char *dir, const char *name)
{
    if (strcmp(dir, "/") == 0)
        ksnprintf(out, VFS_PATH_MAX, "/%s", name);
    else
        ksnprintf(out, VFS_PATH_MAX, "%s/%s", dir, name);
}

/* Tartib: avval ".." , keyin papkalar, keyin fayllar; har guruh ichida nom bo'yicha */
static int compare(const fentry_t *a, const fentry_t *b)
{
    int ga = a->kind == K_PARENT ? 0 : a->kind == K_DIR ? 1 : 2;
    int gb = b->kind == K_PARENT ? 0 : b->kind == K_DIR ? 1 : 2;
    if (ga != gb)
        return ga - gb;
    return strcmp(a->name, b->name);
}

static void load_locked(files_t *f)
{
    f->count = 0;
    f->scroll = 0;
    f->selected = 0;
    vfs_node_t *dir;
    int err = vfs_lookup(f->path, &dir);
    if (err != VFS_OK || dir->type != VFS_DIR) {
        ksnprintf(f->status, sizeof(f->status), "Xato: %s", vfs_strerror(err ? err : VFS_ENOTDIR));
        return;
    }
    if (strcmp(f->path, "/") != 0) {
        strcpy(f->entries[0].name, "..");
        f->entries[0].kind = K_PARENT;
        f->entries[0].size = 0;
        f->count = 1;
    }
    bool in_bin = strcmp(f->path, "/bin") == 0;
    vfs_node_t *n;
    for (uint32_t i = 0; f->count < MAX_ENTRIES && (n = vfs_readdir(dir, i)) != 0; i++) {
        fentry_t *e = &f->entries[f->count++];
        strlcpy(e->name, n->name, sizeof(e->name));
        e->size = n->size;
        if (n->type == VFS_DIR)
            e->kind = K_DIR;
        else if (in_bin)
            e->kind = K_PROGRAM;
        else if (ends_with(n->name, ".bmp"))
            e->kind = K_IMAGE;
        else
            e->kind = K_FILE;
    }
    /* Qo'shib saralash (insertion sort): ro'yxat kichik, oddiy algoritm yetarli */
    for (int i = 1; i < f->count; i++) {
        fentry_t tmp = f->entries[i];
        int j = i - 1;
        while (j >= 0 && compare(&f->entries[j], &tmp) > 0) {
            f->entries[j + 1] = f->entries[j];
            j--;
        }
        f->entries[j + 1] = tmp;
    }
    if (dir->mountpoint && !dir->mount)
        ksnprintf(f->status, sizeof(f->status), "Disk ulanmagan (terminalda: mkfs yoki mount disk)");
    else
        ksnprintf(f->status, sizeof(f->status), "%d ta element", f->count - (f->count && f->entries[0].kind == K_PARENT));
}

/* Papkani o'qish paytida tugunlar o'chirilmasligi uchun — VFS qulfi ostida */
static void load(files_t *f)
{
    vfs_lock();
    load_locked(f);
    vfs_unlock();
}

static void small_icon(surface_t *s, int x, int y, int kind)
{
    switch (kind) {
    case K_PARENT:
        gfx_text(s, x, y, "..", COL_TEXT_DIM, GFX_TRANSPARENT);
        break;
    case K_DIR:
        gfx_fill(s, x, y + 3, 7, 3, RGB(214, 150, 30));
        gfx_fill(s, x, y + 5, 16, 10, RGB(248, 196, 64));
        break;
    case K_PROGRAM:
        gfx_fill(s, x, y + 2, 16, 13, RGB(36, 130, 80));
        gfx_fill(s, x + 6, y + 5, 2, 7, RGB(255, 255, 255));
        gfx_fill(s, x + 8, y + 6, 2, 5, RGB(255, 255, 255));
        gfx_fill(s, x + 10, y + 7, 2, 3, RGB(255, 255, 255));
        break;
    case K_IMAGE:
        gfx_fill(s, x, y + 2, 16, 13, RGB(120, 128, 140));
        gfx_fill(s, x + 1, y + 3, 14, 11, RGB(150, 200, 245));
        gfx_fill(s, x + 1, y + 10, 14, 4, RGB(70, 160, 80));
        break;
    default:
        gfx_fill(s, x + 2, y + 1, 12, 15, RGB(140, 148, 160));
        gfx_fill(s, x + 3, y + 2, 10, 13, RGB(255, 255, 255));
        gfx_hline(s, x + 5, y + 6, 6, RGB(170, 178, 190));
        gfx_hline(s, x + 5, y + 9, 6, RGB(170, 178, 190));
        break;
    }
}

static const char *kind_name(int kind)
{
    switch (kind) {
    case K_DIR:     return "Papka";
    case K_PROGRAM: return "Dastur (ELF)";
    case K_IMAGE:   return "Rasm (BMP)";
    case K_PARENT:  return "";
    default:        return "Fayl";
    }
}

static void redraw(window_t *w)
{
    files_t *f = w->data;
    surface_t *s = &w->surf;
    gfx_fill(s, 0, 0, W, H, RGB(255, 255, 255));

    /* Asboblar paneli */
    gfx_gradient_v(s, 0, 0, W, TOOLBAR_H, RGB(246, 248, 251), RGB(226, 230, 236));
    gfx_hline(s, 0, TOOLBAR_H - 1, W, RGB(196, 202, 212));
    ui_button(s, 6, 6, 84, 24, "Yuqoriga", false);
    ui_button(s, 96, 6, 44, 24, "Uy", false);
    ui_button(s, 146, 6, 54, 24, "Disk", false);
    gfx_fill(s, 208, 6, W - 214, 24, RGB(255, 255, 255));
    gfx_rect(s, 208, 6, W - 214, 24, RGB(180, 186, 196));
    char shown[64];
    size_t len = strlen(f->path), max = (size_t)(W - 226) / FONT_W;
    strlcpy(shown, len > max ? f->path + (len - max) : f->path, sizeof(shown));
    gfx_text(s, 216, 10, shown, COL_TEXT, GFX_TRANSPARENT);

    /* Ustun sarlavhalari */
    gfx_text_bold(s, 34, HEADER_Y, "Nomi", COL_TEXT_DIM);
    gfx_text_bold(s, 330, HEADER_Y, "Hajmi", COL_TEXT_DIM);
    gfx_text_bold(s, 430, HEADER_Y, "Turi", COL_TEXT_DIM);
    gfx_hline(s, 6, LIST_Y - 3, W - 12, RGB(220, 224, 230));

    for (int row = 0; row < VISIBLE_ROWS; row++) {
        int i = f->scroll + row;
        if (i >= f->count)
            break;
        const fentry_t *e = &f->entries[i];
        int y = LIST_Y + row * ROW_H;
        if (i == f->selected)
            gfx_fill(s, 2, y, W - 14, ROW_H, COL_SELECT);
        else if (row & 1)
            gfx_fill(s, 2, y, W - 14, ROW_H, RGB(248, 249, 251));
        small_icon(s, 10, y + 2, e->kind);
        char name[36];
        strlcpy(name, e->name, sizeof(name));
        gfx_text(s, 34, y + 2, name, COL_TEXT, GFX_TRANSPARENT);
        if (e->kind != K_DIR && e->kind != K_PARENT) {
            char size[16];
            if (e->size >= 10240)
                ksnprintf(size, sizeof(size), "%u KB", e->size / 1024);
            else
                ksnprintf(size, sizeof(size), "%u B", e->size);
            gfx_text(s, 330, y + 2, size, COL_TEXT, GFX_TRANSPARENT);
        }
        gfx_text(s, 430, y + 2, kind_name(e->kind), COL_TEXT_DIM, GFX_TRANSPARENT);
    }

    /* O'ngdagi varaqlash chizig'i */
    int track_h = VISIBLE_ROWS * ROW_H;
    gfx_fill(s, W - 10, LIST_Y, 6, track_h, RGB(236, 238, 242));
    if (f->count > VISIBLE_ROWS) {
        int thumb_h = track_h * VISIBLE_ROWS / f->count;
        int thumb_y = LIST_Y + track_h * f->scroll / f->count;
        gfx_round_rect(s, W - 10, thumb_y, 6, thumb_h < 12 ? 12 : thumb_h, RGB(160, 168, 180));
    }

    /* Holat qatori */
    gfx_fill(s, 0, H - STATUS_H, W, STATUS_H, RGB(236, 239, 243));
    gfx_hline(s, 0, H - STATUS_H, W, RGB(206, 212, 220));
    gfx_text(s, 8, H - STATUS_H + 3, f->status, COL_TEXT_DIM, GFX_TRANSPARENT);
    wm_invalidate(w);
}

static void navigate(window_t *w, const char *path)
{
    files_t *f = w->data;
    strlcpy(f->path, path, sizeof(f->path));
    load(f);
    char title[40];
    ksnprintf(title, sizeof(title), "Fayllar - %s", vfs_basename(f->path)[0] ? vfs_basename(f->path) : "/");
    wm_set_title(w, title);
    redraw(w);
}

static void go_up(window_t *w)
{
    files_t *f = w->data;
    char parent[VFS_PATH_MAX];
    strlcpy(parent, f->path, sizeof(parent));
    char *slash = 0;
    for (char *p = parent; *p; p++)
        if (*p == '/')
            slash = p;
    if (!slash || slash == parent)
        strcpy(parent, "/");
    else
        *slash = 0;
    navigate(w, parent);
}

static void open_entry(window_t *w, int i)
{
    files_t *f = w->data;
    if (i < 0 || i >= f->count)
        return;
    fentry_t *e = &f->entries[i];
    char full[VFS_PATH_MAX];
    join(full, f->path, e->name);
    switch (e->kind) {
    case K_PARENT:
        go_up(w);
        return;
    case K_DIR:
        navigate(w, full);
        return;
    case K_PROGRAM:
        app_run_program(full);
        ksnprintf(f->status, sizeof(f->status), "Ishga tushirildi: %s (natija terminalda)", e->name);
        redraw(w);
        return;
    default:
        app_viewer_open(full);
        return;
    }
}

static void ensure_visible(files_t *f)
{
    if (f->selected < f->scroll)
        f->scroll = f->selected;
    if (f->selected >= f->scroll + VISIBLE_ROWS)
        f->scroll = f->selected - VISIBLE_ROWS + 1;
}

static void files_mouse(window_t *w, int type, int x, int y, int buttons)
{
    files_t *f = w->data;
    if (type != GUI_EV_MOUSE_DOWN || !(buttons & 1))
        return;
    if (y < TOOLBAR_H) {
        if (x >= 6 && x < 90) go_up(w);
        else if (x >= 96 && x < 140) navigate(w, "/home/diyorbek");
        else if (x >= 146 && x < 200) navigate(w, "/disk");
        return;
    }
    if (y < LIST_Y || y >= H - STATUS_H)
        return;
    int i = f->scroll + (y - LIST_Y) / ROW_H;
    if (i >= f->count)
        return;
    uint32_t now = wm_ticks();
    if (i == f->last_index && now - f->last_click < 45) {
        f->last_index = -1;
        open_entry(w, i);
        return;
    }
    f->last_index = i;
    f->last_click = now;
    f->selected = i;
    redraw(w);
}

static void files_wheel(window_t *w, int dz)
{
    files_t *f = w->data;
    f->scroll += dz < 0 ? -3 : 3;
    if (f->scroll > f->count - VISIBLE_ROWS)
        f->scroll = f->count - VISIBLE_ROWS;
    if (f->scroll < 0)
        f->scroll = 0;
    redraw(w);
}

static void files_key(window_t *w, char key)
{
    files_t *f = w->data;
    if (key == KEY_UP && f->selected > 0)
        f->selected--;
    else if (key == KEY_DOWN && f->selected < f->count - 1)
        f->selected++;
    else if (key == '\n') {
        open_entry(w, f->selected);
        return;
    } else if (key == '\b') {
        go_up(w);
        return;
    } else
        return;
    ensure_visible(f);
    redraw(w);
}

static void files_destroy(window_t *w)
{
    files_t *f = w->data;
    kfree(f->entries);
    kfree(f);
}

static const window_ops_t files_ops = {
    .mouse = files_mouse,
    .key = files_key,
    .wheel = files_wheel,
    .destroy = files_destroy,
};

void app_files_open(const char *path)
{
    files_t *f = kmalloc(sizeof(files_t));
    if (!f)
        return;
    memset(f, 0, sizeof(*f));
    f->entries = kmalloc(sizeof(fentry_t) * MAX_ENTRIES);
    if (!f->entries) {
        kfree(f);
        return;
    }
    f->last_index = -1;
    window_t *w = wm_create("Fayllar", W, H, &files_ops, f);
    if (!w) {
        files_destroy(&(window_t){ .data = f });
        return;
    }
    navigate(w, path);
}
