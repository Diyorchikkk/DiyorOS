/*
 * cmd_fs.c — fayl tizimi buyruqlari (v0.4).
 *
 *   ls [yo'l]            papka ichidagilar
 *   cd [yo'l]            joriy papkani o'zgartirish (bo'sh: uy papkasi)
 *   pwd                  joriy papka
 *   mkdir <yo'l>         papka yaratish
 *   touch <yo'l>         bo'sh fayl yaratish
 *   rm [-r] <yo'l>       o'chirish (-r: papkani ichidagilari bilan)
 *   cat <fayl>           fayl mazmuni
 *   cp <manba> <nusxa>   nusxa olish
 *   mv <manba> <yangi>   ko'chirish / nomini o'zgartirish
 *   tree [yo'l]          papkalar daraxti
 *   stat <yo'l>          fayl haqida batafsil (inode, vaqtlar)
 *   wc <fayl>            qator, so'z va baytlar soni
 *   df                   fayl tizimi bandligi
 *
 * Barcha buyruqlar faqat VFS funksiyalaridan foydalanadi — qaysi fayl
 * tizimi (ramfs yoki procfs) ekanini bilishmaydi.
 */
#include <stdint.h>
#include <stdbool.h>
#include "shell.h"
#include "vfs.h"
#include "memory.h"
#include "kprintf.h"
#include "vga.h"
#include "string.h"

#define HOME_DIR "/home/diyorbek"

/* ------------------------------ Yordamchilar ------------------------------ */

static void with_color(uint8_t color)
{
    vga_set_color(color, VGA_BLACK);
}

static uint8_t saved_color;
static void save_color(void)    { saved_color = vga_get_color(); }
static void restore_color(void) { vga_set_color(saved_color & 0x0F, saved_color >> 4); }

static void fs_error(const char *cmd, const char *path, int err)
{
    save_color();
    with_color(VGA_LIGHT_RED);
    kprintf("%s: %s: %s\n", cmd, path, vfs_strerror(err));
    restore_color();
}

/* "a b" -> birinchi so'z out ga, qolgani qaytariladi */
/*
 * Keyingi argumentni olish. Bo'sh joyli nomlar uchun (v0.6, FAT32 da uchraydi):
 *   "Mening hujjatim.txt"     — qo'shtirnoq ichida
 *   Mening\ hujjatim.txt      — "\ " bilan (Tab shunday yozadi)
 */
const char *shell_next_arg(const char *s, char *out, int max)
{
    while (*s == ' ')
        s++;
    int i = 0;
    bool quoted = false;
    while (*s && (quoted || *s != ' ')) {
        if (*s == '"') {
            quoted = !quoted;
            s++;
            continue;
        }
        if (*s == '\\' && s[1]) {
            s++;
        }
        if (i < max - 1)
            out[i++] = *s;
        s++;
    }
    out[i] = 0;
    while (*s == ' ')
        s++;
    return s;
}

static const char *take_word(const char *s, char *out, int max)
{
    return shell_next_arg(s, out, max);
}

static void print_time(const rtc_time_t *t)
{
    kprintf("%04u-%02u-%02u %02u:%02u", t->year, t->month, t->day, t->hour, t->minute);
}

static void print_name(vfs_node_t *n)
{
    save_color();
    if (n->type == VFS_DIR) {
        with_color(VGA_LIGHT_BLUE);
        kprintf("%s/", n->name);
    } else if (n->ops->write == 0) {
        with_color(VGA_LIGHT_MAGENTA);        /* faqat o'qiladigan (procfs) */
        kprintf("%s", n->name);
    } else {
        kprintf("%s", n->name);
    }
    restore_color();
}

static uint32_t count_children(vfs_node_t *dir)
{
    uint32_t c = 0;
    while (vfs_readdir(dir, c))
        c++;
    return c;
}

static void ls_line(vfs_node_t *n)
{
    if (n->type == VFS_DIR)
        kprintf("d  %6u ta  ", count_children(n));
    else
        kprintf("-  %6u B   ", n->size);
    print_time(&n->mtime);
    kprintf("  ");
    print_name(n);
    kprintf("\n");
}

/* -------------------------------- Buyruqlar -------------------------------- */

void cmd_ls(const char *args)
{
    char path[VFS_PATH_MAX];
    take_word(args, path, sizeof(path));
    if (!path[0])
        strcpy(path, ".");

    vfs_node_t *n;
    int err = vfs_lookup(path, &n);
    if (err) {
        fs_error("ls", path, err);
        return;
    }
    if (n->type != VFS_DIR) {
        ls_line(n);
        return;
    }

    save_color();
    with_color(VGA_DARK_GREY);
    kprintf("T    HAJMI    O'ZGARTIRILGAN    NOMI\n");
    restore_color();

    uint32_t i = 0, total = 0;
    vfs_node_t *c;
    while ((c = vfs_readdir(n, i++))) {
        ls_line(c);
        total += c->size;
    }
    if (i == 1)
        kprintf("(bo'sh)\n");
    else
        kprintf("Jami: %u ta, %u bayt\n", i - 1, total);
}

void cmd_cd(const char *args)
{
    char path[VFS_PATH_MAX];
    take_word(args, path, sizeof(path));
    if (!path[0] || strcmp(path, "~") == 0)
        strcpy(path, HOME_DIR);
    int err = vfs_chdir(path);
    if (err)
        fs_error("cd", path, err);
}

void cmd_pwd(const char *args)
{
    (void)args;
    char buf[VFS_PATH_MAX];
    vfs_get_path(vfs_cwd(), buf, sizeof(buf));
    kprintf("%s\n", buf);
}

void cmd_mkdir(const char *args)
{
    char path[VFS_PATH_MAX];
    take_word(args, path, sizeof(path));
    if (!path[0]) {
        kprintf("Foydalanish: mkdir <papka>\n");
        return;
    }
    int err = vfs_mkdir(path);
    if (err)
        fs_error("mkdir", path, err);
}

void cmd_touch(const char *args)
{
    char path[VFS_PATH_MAX];
    take_word(args, path, sizeof(path));
    if (!path[0]) {
        kprintf("Foydalanish: touch <fayl>\n");
        return;
    }
    vfs_node_t *n;
    if (vfs_lookup(path, &n) == VFS_OK) {    /* bor bo'lsa — faqat vaqtini yangilaymiz */
        if (n->ops->write)
            vfs_touch_time(n);
        return;
    }
    int err = vfs_create(path, 0);
    if (err)
        fs_error("touch", path, err);
}

void cmd_rm(const char *args)
{
    char word[VFS_PATH_MAX];
    bool recursive = false;
    const char *rest = take_word(args, word, sizeof(word));
    if (strcmp(word, "-r") == 0) {
        recursive = true;
        take_word(rest, word, sizeof(word));
    }
    if (!word[0]) {
        kprintf("Foydalanish: rm [-r] <yo'l>\n");
        return;
    }
    int err = vfs_remove(word, recursive);
    if (err)
        fs_error("rm", word, err);
}

void cmd_cat(const char *args)
{
    char path[VFS_PATH_MAX];
    take_word(args, path, sizeof(path));
    if (!path[0]) {
        kprintf("Foydalanish: cat <fayl>\n");
        return;
    }
    int fd = vfs_open(path, VFS_O_READ);
    if (fd < 0) {
        fs_error("cat", path, fd);
        return;
    }

    char buf[256];
    int32_t n;
    char last = '\n';
    while ((n = vfs_read(fd, buf, sizeof(buf))) > 0)
        for (int32_t i = 0; i < n; i++) {
            char c = buf[i];
            if (c == '\n' || c == '\t' || (c >= 32 && c < 127))
                vga_putc(c);
            else
                vga_putc('.');               /* chop etilmaydigan baytlar */
            last = c;
        }
    if (last != '\n')
        vga_putc('\n');
    vfs_close(fd);
}

/* Faylni nusxalash: manbadan o'qib, yangi faylga yozish */
void cmd_cp(const char *args)
{
    char src[VFS_PATH_MAX], dst[VFS_PATH_MAX];
    const char *rest = take_word(args, src, sizeof(src));
    take_word(rest, dst, sizeof(dst));
    if (!src[0] || !dst[0]) {
        kprintf("Foydalanish: cp <manba> <nusxa>\n");
        return;
    }

    vfs_node_t *d;
    if (vfs_lookup(dst, &d) == VFS_OK && d->type == VFS_DIR) {  /* papkaga — o'sha nom bilan */
        uint32_t len = strlen(dst);
        ksnprintf(dst + len, sizeof(dst) - len, "%s%s",
                  dst[len - 1] == '/' ? "" : "/", vfs_basename(src));
    }

    int in = vfs_open(src, VFS_O_READ);
    if (in < 0) {
        fs_error("cp", src, in);
        return;
    }
    int out = vfs_open(dst, VFS_O_WRITE | VFS_O_CREATE | VFS_O_TRUNC);
    if (out < 0) {
        vfs_close(in);
        fs_error("cp", dst, out);
        return;
    }

    char buf[512];
    int32_t n;
    uint32_t total = 0;
    while ((n = vfs_read(in, buf, sizeof(buf))) > 0) {
        int32_t w = vfs_write(out, buf, n);
        if (w < 0) {
            fs_error("cp", dst, w);
            break;
        }
        total += w;
    }
    vfs_close(in);
    vfs_close(out);
    kprintf("%u bayt nusxalandi\n", total);
}

void cmd_mv(const char *args)
{
    char src[VFS_PATH_MAX], dst[VFS_PATH_MAX];
    const char *rest = take_word(args, src, sizeof(src));
    take_word(rest, dst, sizeof(dst));
    if (!src[0] || !dst[0]) {
        kprintf("Foydalanish: mv <manba> <yangi joy yoki nom>\n");
        return;
    }
    int err = vfs_rename(src, dst);
    if (err)
        fs_error("mv", src, err);
}

static void tree_walk(vfs_node_t *dir, char *prefix, int depth, uint32_t *files, uint32_t *dirs)
{
    if (depth > 8)
        return;
    uint32_t count = count_children(dir);
    for (uint32_t i = 0; i < count; i++) {
        vfs_node_t *c = vfs_readdir(dir, i);
        bool last = (i == count - 1);
        kprintf("%s%s", prefix, last ? "`-- " : "|-- ");
        print_name(c);
        kprintf("\n");
        if (c->type == VFS_DIR) {
            (*dirs)++;
            uint32_t len = strlen(prefix);
            if (len + 5 < 64) {
                strcpy(prefix + len, last ? "    " : "|   ");
                tree_walk(c, prefix, depth + 1, files, dirs);
                prefix[len] = 0;
            }
        } else {
            (*files)++;
        }
    }
}

void cmd_tree(const char *args)
{
    char path[VFS_PATH_MAX];
    take_word(args, path, sizeof(path));
    if (!path[0])
        strcpy(path, ".");
    vfs_node_t *n;
    int err = vfs_lookup(path, &n);
    if (err) {
        fs_error("tree", path, err);
        return;
    }
    if (n->type != VFS_DIR) {
        fs_error("tree", path, VFS_ENOTDIR);
        return;
    }
    char full[VFS_PATH_MAX];
    vfs_get_path(n, full, sizeof(full));
    save_color();
    with_color(VGA_LIGHT_BLUE);
    kprintf("%s\n", full);
    restore_color();

    char prefix[64] = "";
    uint32_t files = 0, dirs = 0;
    tree_walk(n, prefix, 0, &files, &dirs);
    kprintf("\n%u ta papka, %u ta fayl\n", dirs, files);
}

void cmd_stat(const char *args)
{
    char path[VFS_PATH_MAX];
    take_word(args, path, sizeof(path));
    if (!path[0]) {
        kprintf("Foydalanish: stat <yo'l>\n");
        return;
    }
    vfs_node_t *n;
    int err = vfs_lookup(path, &n);
    if (err) {
        fs_error("stat", path, err);
        return;
    }
    char full[VFS_PATH_MAX];
    vfs_get_path(n, full, sizeof(full));

    kprintf("  Yo'l:          %s\n", full);
    kprintf("  Turi:          %s\n", n->type == VFS_DIR ? "papka" : "oddiy fayl");
    if (n->type == VFS_DIR)
        kprintf("  Ichida:        %u ta element\n", count_children(n));
    else
        kprintf("  Hajmi:         %u bayt%s\n", n->size,
                n->generate ? " (mazmuni o'qilganda yaratiladi)" : "");
    if (n->capacity)
        kprintf("  Ajratilgan:    %u bayt (heap da)\n", n->capacity);
    kprintf("  Inode:         %u\n", n->inode);
    kprintf("  Fayl tizimi:   %s%s\n", n->fs_name, n->ops->write ? "" : " (faqat o'qish)");
    kprintf("  Yaratilgan:    ");
    print_time(&n->ctime);
    kprintf("\n  O'zgartirilgan: ");
    print_time(&n->mtime);
    kprintf("\n  Ochiq fd lar:  %u\n", n->open_count);
}

void cmd_wc(const char *args)
{
    char path[VFS_PATH_MAX];
    take_word(args, path, sizeof(path));
    if (!path[0]) {
        kprintf("Foydalanish: wc <fayl>\n");
        return;
    }
    int fd = vfs_open(path, VFS_O_READ);
    if (fd < 0) {
        fs_error("wc", path, fd);
        return;
    }
    uint32_t lines = 0, words = 0, bytes = 0;
    bool in_word = false;
    char buf[256];
    int32_t n;
    while ((n = vfs_read(fd, buf, sizeof(buf))) > 0)
        for (int32_t i = 0; i < n; i++) {
            char c = buf[i];
            bytes++;
            if (c == '\n')
                lines++;
            bool space = (c == ' ' || c == '\n' || c == '\t');
            if (!space && !in_word)
                words++;
            in_word = !space;
        }
    vfs_close(fd);
    kprintf("%u qator, %u so'z, %u bayt  %s\n", lines, words, bytes, path);
}

/* ramfs bo'ylab yurib, fayllar va hajmni sanash (mount nuqtalariga kirmaymiz) */
static void df_walk(vfs_node_t *dir, uint32_t *files, uint32_t *dirs, uint32_t *bytes, uint32_t *alloc)
{
    vfs_node_t *c;
    for (uint32_t i = 0; (c = dir->ops->readdir(dir, i)); i++) {
        *alloc += sizeof(vfs_node_t) + c->capacity;
        if (c->type == VFS_DIR) {
            (*dirs)++;
            if (!c->mount)
                df_walk(c, files, dirs, bytes, alloc);
        } else {
            (*files)++;
            *bytes += c->size;
        }
    }
}

void cmd_df(const char *args)
{
    (void)args;
    uint32_t files = 0, dirs = 0, bytes = 0, alloc = 0;
    df_walk(vfs_root(), &files, &dirs, &bytes, &alloc);
    heap_stats_t hs;
    heap_get_stats(&hs);

    kprintf("Fayl tizimi  Ulangan   Tarkibi\n");
    kprintf("ramfs        /         %u papka, %u fayl, %u bayt ma'lumot\n", dirs, files, bytes);
    kprintf("procfs       /proc     virtual, xotira egallamaydi\n");
    fat32_stats_t st;
    fat32_stats(&st);
    if (st.mounted) {
        kprintf("fat32        /disk     \"%s\": %u KB band, %u MB bo'sh (diskda)\n", st.label,
                (st.total_clusters - st.free_clusters) * (st.cluster_bytes / 512) / 2,
                st.free_clusters / (1024 * 1024 / st.cluster_bytes));
    }
    kprintf("\n");
    kprintf("ramfs heap dan egallagan joy: %u bayt (tugunlar + fayl buferlari)\n", alloc);
    kprintf("Bo'sh RAM: %u KB (fayllar shu hisobidan o'sishi mumkin)\n",
            (pmm_total_frames() - pmm_used_frames()) * 4);
}

/* echo matn > fayl   |   echo matn >> fayl */
bool shell_redirect(const char *args)
{
    const char *gt = 0;
    for (const char *p = args; *p; p++)
        if (*p == '>') {
            gt = p;
            break;
        }
    if (!gt)
        return false;

    bool append = (gt[1] == '>');
    char path[VFS_PATH_MAX];
    take_word(gt + (append ? 2 : 1), path, sizeof(path));
    if (!path[0]) {
        kprintf("echo: '>' dan keyin fayl nomi kerak\n");
        return true;
    }

    uint32_t len = gt - args;
    while (len > 0 && args[len - 1] == ' ')
        len--;

    uint32_t flags = VFS_O_WRITE | VFS_O_CREATE | (append ? VFS_O_APPEND : VFS_O_TRUNC);
    int fd = vfs_open(path, flags);
    if (fd < 0) {
        fs_error("echo", path, fd);
        return true;
    }
    int32_t w = vfs_write(fd, args, len);
    if (w >= 0)
        w = vfs_write(fd, "\n", 1);
    if (w < 0)
        fs_error("echo", path, w);
    vfs_close(fd);
    return true;
}
