/*
 * editor.c — to'liq ekranli matn muharriri (edit buyrug'i).
 *
 * Ishlash prinsipi:
 *   - Fayl butunlay xotiradagi buferga o'qiladi (text, len).
 *   - Kursor — buferdagi indeks (cur). Harf yozilsa, shu joyga qo'shiladi
 *     (qolgan matn memmove bilan o'ngga suriladi), Backspace — chapdagini o'chiradi.
 *   - Har bir tugmadan keyin ekran qayta chiziladi: 1-23 qatorlarda matn,
 *     24-qatorda holat qatori. Uzun fayllar uchun "top_line" (qaysi qatordan
 *     ko'rsatish) va uzun qatorlar uchun "left_col" (gorizontal surish) bor.
 *   - Saqlash: vfs_open(TRUNC) + vfs_write — ya'ni VFS orqali, istalgan
 *     yoziladigan fayl tizimiga.
 *
 * Tugmalar: strelkalar, Home/End (Fn+chap/o'ng), PgUp/PgDn (Fn+yuqori/past),
 *           Backspace, Delete (Fn+Backspace), Tab (4 bo'sh joy), ESC — menyu.
 */
#include <stdint.h>
#include <stdbool.h>
#include "shell.h"
#include "vfs.h"
#include "vga.h"
#include "drivers.h"
#include "memory.h"
#include "kprintf.h"
#include "string.h"

#define EDIT_MAX    (60 * 1024)
#define TEXT_FIRST  1                      /* 0-qator — umumiy status bar */
#define TEXT_LINES  (VGA_HEIGHT - 2)       /* 23 qator matn */
#define INFO_ROW    (VGA_HEIGHT - 1)       /* 24-qator — muharrir holati */

typedef struct {
    char     path[VFS_PATH_MAX];
    char    *text;
    uint32_t len;
    uint32_t cur;          /* kursor: buferdagi indeks */
    uint32_t top_line;     /* ekrandagi birinchi qator raqami */
    uint32_t left_col;     /* gorizontal surish */
    bool     modified;
    bool     readonly;
    char     message[80];  /* holat qatorida vaqtincha ko'rsatiladigan xabar */
} editor_t;

/* ------------------------------ Matn bo'ylab ------------------------------ */

static uint32_t line_start(editor_t *e, uint32_t pos)
{
    while (pos > 0 && e->text[pos - 1] != '\n')
        pos--;
    return pos;
}

static uint32_t line_end(editor_t *e, uint32_t pos)
{
    while (pos < e->len && e->text[pos] != '\n')
        pos++;
    return pos;
}

/* Kursor qaysi qator va ustunda */
static void cursor_line_col(editor_t *e, uint32_t *line, uint32_t *col)
{
    uint32_t l = 0;
    for (uint32_t i = 0; i < e->cur; i++)
        if (e->text[i] == '\n')
            l++;
    *line = l;
    *col = e->cur - line_start(e, e->cur);
}

static uint32_t total_lines(editor_t *e)
{
    uint32_t l = 1;
    for (uint32_t i = 0; i < e->len; i++)
        if (e->text[i] == '\n')
            l++;
    return l;
}

/* n-qatorning boshlanish indeksi (yo'q bo'lsa — len) */
static uint32_t nth_line(editor_t *e, uint32_t n)
{
    uint32_t pos = 0;
    while (n > 0 && pos < e->len) {
        if (e->text[pos] == '\n')
            n--;
        pos++;
    }
    return n > 0 ? e->len : pos;
}

/* -------------------------------- Tahrirlash -------------------------------- */

static void insert_char(editor_t *e, char c)
{
    if (e->len >= EDIT_MAX - 1) {
        strcpy(e->message, "Fayl juda katta (maksimum 60 KB)");
        return;
    }
    memmove(e->text + e->cur + 1, e->text + e->cur, e->len - e->cur);
    e->text[e->cur++] = c;
    e->len++;
    e->modified = true;
}

static void delete_at(editor_t *e, uint32_t pos)
{
    if (pos >= e->len)
        return;
    memmove(e->text + pos, e->text + pos + 1, e->len - pos - 1);
    e->len--;
    e->modified = true;
}

/* Yuqori/pastga: qo'shni qatorning o'sha ustuniga (sig'masa — oxiriga) */
static void move_vertical(editor_t *e, int delta)
{
    uint32_t line, col;
    cursor_line_col(e, &line, &col);
    int32_t target = (int32_t)line + delta;
    if (target < 0)
        target = 0;
    uint32_t last = total_lines(e) - 1;
    if ((uint32_t)target > last)
        target = last;

    uint32_t start = nth_line(e, target);
    uint32_t end = line_end(e, start);
    e->cur = (start + col <= end) ? start + col : end;
}

/* --------------------------------- Chizish --------------------------------- */

static void draw_info(editor_t *e, const char *override)
{
    char line[VGA_WIDTH + 1];
    uint32_t l, c;
    cursor_line_col(e, &l, &c);

    if (override) {
        ksnprintf(line, sizeof(line), " %s", override);
    } else {
        char right[40];
        ksnprintf(right, sizeof(right), "Qator %u/%u, ustun %u   ESC: menyu ",
                  l + 1, total_lines(e), c + 1);
        char left[48];
        ksnprintf(left, sizeof(left), " %s%s%s", vfs_basename(e->path),
                  e->modified ? " [o'zgartirildi]" : "",
                  e->readonly ? " [faqat o'qish]" : "");
        if (e->message[0]) {
            ksnprintf(left, sizeof(left), " %s", e->message);
            e->message[0] = 0;
        }
        int ll = strlen(left), rl = strlen(right);
        int i = 0;
        for (; i < ll && i < VGA_WIDTH; i++)
            line[i] = left[i];
        for (; i < VGA_WIDTH - rl; i++)
            line[i] = ' ';
        for (int j = 0; j < rl && i < VGA_WIDTH; j++)
            line[i++] = right[j];
        line[VGA_WIDTH] = 0;
    }

    int x = 0;
    for (; line[x] && x < VGA_WIDTH; x++)
        vga_put_at(x, INFO_ROW, line[x], VGA_BLACK, VGA_LIGHT_GREY);
    for (; x < VGA_WIDTH; x++)
        vga_put_at(x, INFO_ROW, ' ', VGA_BLACK, VGA_LIGHT_GREY);
}

static void render(editor_t *e)
{
    uint32_t line, col;
    cursor_line_col(e, &line, &col);

    /* Kursor ekranda ko'rinadigan bo'lsin */
    if (line < e->top_line)
        e->top_line = line;
    if (line >= e->top_line + TEXT_LINES)
        e->top_line = line - TEXT_LINES + 1;
    if (col < e->left_col)
        e->left_col = col;
    if (col >= e->left_col + VGA_WIDTH)
        e->left_col = col - VGA_WIDTH + 1;

    uint32_t pos = nth_line(e, e->top_line);
    uint32_t lines = total_lines(e);

    for (int row = 0; row < TEXT_LINES; row++) {
        int y = TEXT_FIRST + row;
        if (e->top_line + row >= lines) {          /* fayl tugagan — "~" (vim uslubida) */
            vga_put_at(0, y, '~', VGA_DARK_GREY, VGA_BLACK);
            for (int x = 1; x < VGA_WIDTH; x++)
                vga_put_at(x, y, ' ', VGA_LIGHT_GREY, VGA_BLACK);
            continue;
        }
        uint32_t end = line_end(e, pos);
        for (int x = 0; x < VGA_WIDTH; x++) {
            uint32_t i = pos + e->left_col + x;
            char c = (i < end) ? e->text[i] : ' ';
            if (c == '\t')
                c = ' ';
            vga_put_at(x, y, c, VGA_LIGHT_GREY, VGA_BLACK);
        }
        pos = end + 1;
    }

    draw_info(e, 0);
    vga_move_hw_cursor(col - e->left_col, TEXT_FIRST + (line - e->top_line));
}

/* ------------------------------ Fayl bilan ish ------------------------------ */

static bool save(editor_t *e)
{
    int fd = vfs_open(e->path, VFS_O_WRITE | VFS_O_CREATE | VFS_O_TRUNC);
    if (fd < 0) {
        ksnprintf(e->message, sizeof(e->message), "Saqlab bo'lmadi: %s", vfs_strerror(fd));
        return false;
    }
    int32_t w = vfs_write(fd, e->text, e->len);
    vfs_close(fd);
    if (w < 0 || (uint32_t)w != e->len) {
        ksnprintf(e->message, sizeof(e->message), "Saqlab bo'lmadi: %s", vfs_strerror(w));
        return false;
    }
    e->modified = false;
    ksnprintf(e->message, sizeof(e->message), "Saqlandi: %u bayt", e->len);
    return true;
}

static int load(editor_t *e)
{
    vfs_node_t *n;
    int err = vfs_lookup(e->path, &n);
    if (err == VFS_ENOENT) {
        strcpy(e->message, "Yangi fayl (saqlaganda yaratiladi)");
        return VFS_OK;
    }
    if (err)
        return err;
    if (n->type == VFS_DIR)
        return VFS_EISDIR;
    e->readonly = (n->ops->write == 0);

    int fd = vfs_open(e->path, VFS_O_READ);
    if (fd < 0)
        return fd;
    int32_t r;
    while ((r = vfs_read(fd, e->text + e->len, EDIT_MAX - 1 - e->len)) > 0)
        e->len += r;
    vfs_close(fd);
    if (e->len >= EDIT_MAX - 1)
        return VFS_EFBIG;
    return VFS_OK;
}

/* ESC menyusi. true — muharrirdan chiqish */
static bool menu(editor_t *e)
{
    draw_info(e, e->readonly
        ? "Q: chiqish  |  boshqa tugma: davom etish"
        : "S: saqlab chiqish | W: saqlash | Q: saqlamay chiqish | boshqa: davom");
    char c = keyboard_getchar();
    if (c == 'q' || c == 'Q')
        return true;
    if (!e->readonly && (c == 's' || c == 'S'))
        return save(e);
    if (!e->readonly && (c == 'w' || c == 'W'))
        save(e);
    return false;
}

void cmd_edit(const char *args)
{
    while (*args == ' ')
        args++;
    if (!*args) {
        kprintf("Foydalanish: edit <fayl>   (yo'q bo'lsa, saqlaganda yaratiladi)\n");
        return;
    }

    editor_t *e = kmalloc(sizeof(editor_t));
    char *text = kmalloc(EDIT_MAX);
    if (!e || !text) {
        kprintf("edit: xotira yetmadi\n");
        kfree(e);
        kfree(text);
        return;
    }
    memset(e, 0, sizeof(*e));
    e->text = text;
    shell_next_arg(args, e->path, VFS_PATH_MAX);   /* "bo'sh joyli nom" ham bo'lishi mumkin */

    int err = load(e);
    if (err) {
        kprintf("edit: %s: %s\n", e->path, vfs_strerror(err));
        kfree(text);
        kfree(e);
        return;
    }

    vga_screen_save();
    bool done = false;
    while (!done) {
        render(e);
        char c = keyboard_getchar();
        uint32_t s;

        switch (c) {
        case KEY_ESC:      done = menu(e); break;
        case KEY_LEFT:     if (e->cur > 0) e->cur--; break;
        case KEY_RIGHT:    if (e->cur < e->len) e->cur++; break;
        case KEY_UP:
        case KEY_SCROLL_UP:   move_vertical(e, -1); break;
        case KEY_DOWN:
        case KEY_SCROLL_DOWN: move_vertical(e, 1); break;
        case KEY_PGUP:     move_vertical(e, -TEXT_LINES); break;
        case KEY_PGDN:     move_vertical(e, TEXT_LINES); break;
        case KEY_HOME:     e->cur = line_start(e, e->cur); break;
        case KEY_END:      e->cur = line_end(e, e->cur); break;
        case KEY_DELETE:   if (!e->readonly) delete_at(e, e->cur); break;
        case '\b':
            if (!e->readonly && e->cur > 0) {
                e->cur--;
                delete_at(e, e->cur);
            }
            break;
        case '\t':
            if (!e->readonly)
                for (s = 0; s < 4; s++)
                    insert_char(e, ' ');
            break;
        default:
            if (e->readonly) {
                strcpy(e->message, "Bu fayl faqat o'qish uchun");
                break;
            }
            if (c == '\n' || (c >= 32 && c < 127))
                insert_char(e, c);
            break;
        }
    }

    vga_screen_restore();
    if (e->modified)
        kprintf("edit: o'zgarishlar saqlanmadi\n");
    else if (e->message[0])
        kprintf("edit: %s (%s)\n", e->message, e->path);
    kfree(text);
    kfree(e);
}
