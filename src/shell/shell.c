/*
 * shell.c — oddiy buyruqlar qatori (command line interpreter).
 *
 * Ishlash tsikli (REPL):  o'qish -> tahlil qilish -> bajarish -> takrorlash
 * Buyruqlar jadvalda saqlanadi: nomi, tavsifi va bajaradigan funksiyasi.
 * Yangi buyruq qo'shish uchun funksiya yozib, jadvalga bitta qator qo'shish kifoya.
 */
#include <stdint.h>
#include <stdbool.h>
#include "config.h"
#include "kernel.h"
#include "vga.h"
#include "kprintf.h"
#include "drivers.h"
#include "string.h"
#include "io.h"
#include "shell.h"
#include "vfs.h"
#include "task.h"
#include "process.h"

#define LINE_MAX    256
#define HISTORY_MAX 10

static char history[HISTORY_MAX][LINE_MAX];
static int  history_count = 0;
static uint8_t text_color = VGA_LIGHT_GREY;   /* 'color' buyrug'i o'zgartiradi */

static void normal(void)
{
    vga_set_color(text_color, VGA_BLACK);
}

/* ------------------------- Yordamchi funksiyalar ------------------------- */

static const char *skip_spaces(const char *s)
{
    while (*s == ' ')
        s++;
    return s;
}

/* Satrdan butun son o'qish. Muvaffaqiyatli bo'lsa true qaytaradi. */
static bool parse_int(const char **p, int32_t *out)
{
    const char *s = skip_spaces(*p);
    bool neg = false;
    if (*s == '-' || *s == '+') {
        neg = (*s == '-');
        s++;
    }
    if (*s < '0' || *s > '9')
        return false;
    int32_t v = 0;
    while (*s >= '0' && *s <= '9')
        v = v * 10 + (*s++ - '0');
    *out = neg ? -v : v;
    *p = s;
    return true;
}

static void history_add(const char *line)
{
    if (!line[0])
        return;
    if (history_count > 0 && strcmp(history[history_count - 1], line) == 0)
        return;
    if (history_count == HISTORY_MAX) {
        for (int i = 1; i < HISTORY_MAX; i++)
            strcpy(history[i - 1], history[i]);
        history_count--;
    }
    strncpy(history[history_count], line, LINE_MAX - 1);
    history[history_count][LINE_MAX - 1] = 0;
    history_count++;
}

static void prompt(void)
{
    char path[VFS_PATH_MAX];
    vfs_get_path(vfs_cwd(), path, sizeof(path));
    const char *home = "/home/diyorbek";
    size_t hl = strlen(home);

    vga_set_color(VGA_LIGHT_GREEN, VGA_BLACK);
    kprintf("%s", OS_NAME);
    vga_set_color(VGA_WHITE, VGA_BLACK);
    kprintf(":");
    vga_set_color(VGA_LIGHT_BLUE, VGA_BLACK);
    if (strncmp(path, home, hl) == 0 && (path[hl] == 0 || path[hl] == '/'))
        kprintf("~%s", path + hl);              /* uy papkasi "~" bilan qisqartiriladi */
    else
        kprintf("%s", path);
    vga_set_color(VGA_WHITE, VGA_BLACK);
    kprintf("> ");
    normal();
}

static void tab_complete(char *buf, int *len);

/* Klaviaturadan bitta qator o'qish: backspace va tarix (yuqori/pastki strelka) bilan */
static void read_line(char *buf)
{
    int len = 0;
    int hpos = history_count;

    for (;;) {
        char c = keyboard_getchar();

        bool scroll_key = (c == KEY_PGUP || c == KEY_PGDN ||
                           c == KEY_SCROLL_UP || c == KEY_SCROLL_DOWN);
        if (!scroll_key && vga_view_offset()) {   /* boshqa tugma: jonli ekranga qaytamiz */
            vga_scroll_view(-vga_view_offset());
            status_bar_refresh();
        }
        if (c == '\n') {
            vga_putc('\n');
            buf[len] = 0;
            return;
        }
        if (c == '\b') {
            if (len > 0) {
                len--;
                vga_putc('\b');
            }
            continue;
        }
        if (scroll_key) {
            int page = VGA_HEIGHT - 2;   /* bir sahifa: 23 qator */
            if (c == KEY_PGUP)        vga_scroll_view(page);
            if (c == KEY_PGDN)        vga_scroll_view(-page);
            if (c == KEY_SCROLL_UP)   vga_scroll_view(1);
            if (c == KEY_SCROLL_DOWN) vga_scroll_view(-1);
            status_bar_refresh();
            continue;
        }
        if (c == KEY_UP || c == KEY_DOWN) {
            if (c == KEY_UP && hpos > 0)
                hpos--;
            else if (c == KEY_DOWN && hpos < history_count)
                hpos++;
            else
                continue;
            while (len > 0) {           /* joriy yozilganni o'chiramiz */
                vga_putc('\b');
                len--;
            }
            if (hpos < history_count) {
                strcpy(buf, history[hpos]);
                len = strlen(buf);
                vga_puts(buf);
            }
            continue;
        }
        if (c == KEY_CTRL_C) {          /* Ctrl+C: qatorni bekor qilish */
            kprintf("^C\n");
            len = 0;
            buf[0] = 0;
            hpos = history_count;
            prompt();
            continue;
        }
        if (c == '\t') {
            buf[len] = 0;
            tab_complete(buf, &len);
            continue;
        }
        if (c >= 32 && c < 127 && len < LINE_MAX - 1) {
            buf[len++] = c;
            vga_putc(c);
        }
    }
}

/* ------------------------------ Buyruqlar ------------------------------- */

static void cmd_help(const char *args);

static void cmd_clear(const char *args)
{
    (void)args;
    vga_clear();
}

static void cmd_echo(const char *args)
{
    if (shell_redirect(args))      /* echo matn > fayl  yoki  >> fayl */
        return;
    kprintf("%s\n", args);
}

static void cmd_about(const char *args)
{
    (void)args;
    vga_set_color(VGA_LIGHT_CYAN, VGA_BLACK);
    kprintf("%s v%s\n", OS_NAME, OS_VERSION);
    normal();
    kprintf("  Muallif:      %s\n", OS_AUTHOR);
    kprintf("  Arxitektura:  %s\n", OS_ARCH);
    kprintf("  Til:          C va GNU Assembly\n");
    kprintf("  Bootloader:   Multiboot (GRUB / QEMU)\n");
}

static void cmd_uptime(const char *args)
{
    (void)args;
    uint32_t s = timer_seconds();
    kprintf("Ishlash vaqti: %u soat %u daqiqa %u soniya (%u tick)\n",
            s / 3600, (s / 60) % 60, s % 60, timer_ticks());
}

static void cmd_time(const char *args)
{
    (void)args;
    rtc_time_t t;
    rtc_read(&t);
    kprintf("Sana: %04u-%02u-%02u   Vaqt: %02u:%02u:%02u\n",
            t.year, t.month, t.day, t.hour, t.minute, t.second);
}

static void cmd_whoami(const char *args)
{
    (void)args;
    vga_set_color(VGA_LIGHT_CYAN, VGA_BLACK);
    kprintf("Mamasoliyev Diyorbek Abdujalil o'g'li\n");
    normal();
    kprintf("  Universitet:  TATU (Toshkent axborot texnologiyalari universiteti)\n");
    kprintf("  Fakultet:     Dasturiy injiniring, 3-kurs\n");
    kprintf("  Guruh:        SRM002-2\n");
    kprintf("  GitHub:       github.com/Diyorchikkk\n");
}

static void cmd_calc(const char *args)
{
    const char *p = args;
    int32_t a, b;

    if (!parse_int(&p, &a)) goto usage;
    p = skip_spaces(p);
    char op = *p++;
    if (!parse_int(&p, &b)) goto usage;

    switch (op) {
    case '+': kprintf("= %d\n", a + b); return;
    case '-': kprintf("= %d\n", a - b); return;
    case '*': kprintf("= %d\n", a * b); return;
    case '/':
    case '%':
        if (b == 0) {
            vga_set_color(VGA_LIGHT_RED, VGA_BLACK);
            kprintf("Xato: 0 ga bo'lib bo'lmaydi\n");
            normal();
            return;
        }
        kprintf("= %d\n", op == '/' ? a / b : a % b);
        return;
    }
usage:
    kprintf("Foydalanish: calc <son> <+ - * / %%> <son>   masalan: calc 12 * 7\n");
}

static void cmd_color(const char *args)
{
    const char *p = args;
    int32_t c;
    if (!parse_int(&p, &c) || c < 1 || c > 15) {
        kprintf("Foydalanish: color <1-15>\n");
        for (int i = 1; i < 16; i++) {
            vga_set_color(i, VGA_BLACK);
            kprintf("%d ", i);
        }
        normal();
        kprintf("\n");
        return;
    }
    text_color = (uint8_t)c;
    normal();
    kprintf("Matn rangi o'zgartirildi.\n");
}

static void cmd_history(const char *args)
{
    (void)args;
    for (int i = 0; i < history_count; i++)
        kprintf("  %d  %s\n", i + 1, history[i]);
}

static void cmd_sleep(const char *args)
{
    const char *p = args;
    int32_t s;
    if (!parse_int(&p, &s) || s < 1 || s > 60) {
        kprintf("Foydalanish: sleep <1-60 soniya>\n");
        return;
    }
    kprintf("%d soniya kutilmoqda...\n", s);
    timer_sleep_ms((uint32_t)s * 1000);
    kprintf("Tayyor.\n");
}

/* Ataylab 0 ga bo'lib, exception (uzilish) qanday ushlanishini ko'rsatadi */
static void cmd_crash(const char *args)
{
    (void)args;
    kprintf("0 ga bo'lish sinovi... (Exception #0 kutilmoqda)\n");
    volatile int a = 1, b = 0;
    a = a / b;
}

static void cmd_reboot(const char *args)
{
    (void)args;
    kprintf("Qayta yuklanmoqda...\n");
    while (inb(0x64) & 0x02)     /* klaviatura kontrolleri tayyor bo'lguncha */
        ;
    outb(0x64, 0xFE);            /* CPU ga reset signali */
    for (;;)
        __asm__ volatile ("hlt");
}

static void cmd_shutdown(const char *args)
{
    (void)args;
    kprintf("Tizim o'chirilmoqda...\n");
    outw(0x604, 0x2000);         /* QEMU (yangi versiyalar) ACPI o'chirish porti */
    outw(0xB004, 0x2000);        /* Bochs / eski QEMU */
    __asm__ volatile ("cli");
    kprintf("Kompyuterni endi o'chirishingiz mumkin.\n");
    for (;;)
        __asm__ volatile ("hlt");
}

/* Buyruqlar jadvali */
typedef enum { CAT_GENERAL, CAT_FILES, CAT_MEMORY, CAT_TASKS, CAT_SYSTEM, CAT_COUNT } category_t;

static const char *category_names[CAT_COUNT] = {
    "Umumiy", "Fayllar (v0.4)", "Xotira (v0.2)", "Vazifalar (v0.3)", "Tizim va sinov"
};

typedef struct {
    const char *name;
    category_t  category;
    const char *description;
    void (*fn)(const char *args);
} command_t;

static const command_t commands[] = {
    { "help",      CAT_GENERAL, "Buyruqlar ro'yxati. help <buyruq> - batafsil",   cmd_help      },
    { "clear",     CAT_GENERAL, "Ekranni tozalash",                                cmd_clear     },
    { "echo",      CAT_GENERAL, "Matn chiqarish: echo salom | echo matn > fayl",   cmd_echo      },
    { "about",     CAT_GENERAL, "OS haqida ma'lumot",                              cmd_about     },
    { "whoami",    CAT_GENERAL, "Muallif haqida",                                  cmd_whoami    },
    { "time",      CAT_GENERAL, "Sana va vaqt (CMOS soati)",                       cmd_time      },
    { "uptime",    CAT_GENERAL, "Tizim qancha vaqtdan beri ishlayapti",            cmd_uptime    },
    { "calc",      CAT_GENERAL, "Kalkulyator: calc 12 * 7",                        cmd_calc      },
    { "color",     CAT_GENERAL, "Matn rangi: color 14",                            cmd_color     },
    { "history",   CAT_GENERAL, "Kiritilgan buyruqlar tarixi",                     cmd_history   },
    { "sleep",     CAT_GENERAL, "N soniya kutish: sleep 3",                        cmd_sleep     },

    { "ls",        CAT_FILES,   "Papka ichidagilar: ls [yo'l]",                    cmd_ls        },
    { "cd",        CAT_FILES,   "Papkaga o'tish: cd /etc | cd .. | cd (uyga)",     cmd_cd        },
    { "pwd",       CAT_FILES,   "Joriy papka",                                     cmd_pwd       },
    { "cat",       CAT_FILES,   "Fayl mazmuni: cat salom.txt",                     cmd_cat       },
    { "edit",      CAT_FILES,   "Matn muharriri: edit fayl.txt",                   cmd_edit      },
    { "mkdir",     CAT_FILES,   "Papka yaratish: mkdir loyiha",                    cmd_mkdir     },
    { "touch",     CAT_FILES,   "Bo'sh fayl yaratish: touch a.txt",                cmd_touch     },
    { "rm",        CAT_FILES,   "O'chirish: rm fayl | rm -r papka",                cmd_rm        },
    { "cp",        CAT_FILES,   "Nusxa olish: cp a.txt b.txt",                     cmd_cp        },
    { "mv",        CAT_FILES,   "Ko'chirish / nomini o'zgartirish: mv a.txt b.txt", cmd_mv       },
    { "tree",      CAT_FILES,   "Papkalar daraxti: tree /",                        cmd_tree      },
    { "stat",      CAT_FILES,   "Fayl haqida batafsil (inode, vaqtlar)",           cmd_stat      },
    { "wc",        CAT_FILES,   "Qator, so'z, bayt soni: wc salom.txt",            cmd_wc        },
    { "df",        CAT_FILES,   "Fayl tizimlari bandligi",                         cmd_df        },

    { "mem",       CAT_MEMORY,  "Xotira holati: RAM, paging, heap",                cmd_mem       },
    { "memmap",    CAT_MEMORY,  "BIOS xotira xaritasi",                            cmd_memmap    },
    { "alloc",     CAT_MEMORY,  "Heap dan xotira olish: alloc 100",                cmd_alloc     },
    { "free",      CAT_MEMORY,  "Xotirani qaytarish: free 0xD0000018",             cmd_free      },
    { "heap",      CAT_MEMORY,  "Heap bloklari ro'yxati",                          cmd_heap      },
    { "memtest",   CAT_MEMORY,  "Heap ni avtomatik sinash",                        cmd_memtest   },
    { "translate", CAT_MEMORY,  "Virtual -> fizik: translate 0xD0000000",          cmd_translate },

    { "ps",        CAT_TASKS,   "Vazifalar ro'yxati va CPU ulushi",                cmd_ps        },
    { "spawn",     CAT_TASKS,   "Fon vazifasi: spawn spinner | primes | busy",     cmd_spawn     },
    { "kill",      CAT_TASKS,   "Vazifani to'xtatish: kill 3",                     cmd_kill      },
    { "sched",     CAT_TASKS,   "Scheduler statistikasi",                          cmd_sched     },

    { "crash",     CAT_SYSTEM,  "Exception sinovi (0 ga bo'lish)",                 cmd_crash     },
    { "pagefault", CAT_SYSTEM,  "Page Fault sinovi: pagefault | pagefault null",   cmd_pagefault },
    { "reboot",    CAT_SYSTEM,  "Qayta yuklash",                                   cmd_reboot    },
    { "shutdown",  CAT_SYSTEM,  "O'chirish",                                       cmd_shutdown  },
};
#define COMMAND_COUNT (sizeof(commands) / sizeof(commands[0]))

static void cmd_help(const char *args)
{
    args = skip_spaces(args);
    if (*args) {                                /* help <buyruq> */
        char bin[VFS_PATH_MAX];
        vfs_node_t *prog;
        ksnprintf(bin, sizeof(bin), "/bin/%s", args);
        if (vfs_lookup(bin, &prog) == VFS_OK) {
            kprintf("%s - %s dagi dastur (ring 3 da, o'z xotirasida ishlaydi), %u bayt\n",
                    args, bin, prog->size);
            return;
        }
        for (unsigned i = 0; i < COMMAND_COUNT; i++)
            if (strcmp(args, commands[i].name) == 0) {
                vga_set_color(VGA_YELLOW, VGA_BLACK);
                kprintf("%s", commands[i].name);
                normal();
                kprintf(" - %s\n", commands[i].description);
                return;
            }
        kprintf("help: '%s' degan buyruq yo'q\n", args);
        return;
    }

    for (int cat = 0; cat < CAT_COUNT; cat++) {
        vga_set_color(VGA_LIGHT_CYAN, VGA_BLACK);
        kprintf("%s:\n ", category_names[cat]);
        vga_set_color(VGA_YELLOW, VGA_BLACK);
        int col = 1;
        for (unsigned i = 0; i < COMMAND_COUNT; i++) {
            if (commands[i].category != (category_t)cat)
                continue;
            int w = strlen(commands[i].name) + 1;
            if (col + w > VGA_WIDTH - 1) {
                kprintf("\n ");
                col = 1;
            }
            kprintf(" %s", commands[i].name);
            col += w;
        }
        kprintf("\n");
    }
    normal();
    vga_set_color(VGA_LIGHT_CYAN, VGA_BLACK);
    kprintf("Dasturlar (/bin, v0.5):\n ");
    vga_set_color(VGA_YELLOW, VGA_BLACK);
    vfs_node_t *bin, *c;
    if (vfs_lookup("/bin", &bin) == VFS_OK)
        for (uint32_t i = 0; (c = vfs_readdir(bin, i)); i++)
            kprintf(" %s", c->name);
    normal();
    kprintf("  (fonda: nom &, to'xtatish: Ctrl+C)\n");
    kprintf("Batafsil: help <buyruq>.  Tab - to'ldirish, strelkalar - tarix,\n");
    kprintf("sichqoncha g'ildiragi / Fn+strelka - ekranni varaqlash.\n");
}

/* --------------------------- Tab bilan to'ldirish --------------------------- */

#define MAX_MATCHES 48

static void show_line_again(const char *buf)
{
    prompt();
    vga_puts(buf);
}

/*
 * Birinchi so'z — buyruq nomi, keyingilari — fayl yo'li.
 * Bitta mos kelsa — to'liq yoziladi; bir nechta bo'lsa — umumiy qismi
 * yoziladi, umumiy qism bo'lmasa — variantlar ro'yxati chiqariladi.
 */
static void tab_complete(char *buf, int *len)
{
    int start = *len;
    while (start > 0 && buf[start - 1] != ' ')
        start--;
    bool is_command = true;
    for (int i = 0; i < start; i++)
        if (buf[i] != ' ')
            is_command = false;

    const char *word = buf + start;
    const char *names[MAX_MATCHES];
    bool is_dir[MAX_MATCHES];
    int count = 0;
    const char *prefix;

    if (is_command) {
        prefix = word;
        for (unsigned i = 0; i < COMMAND_COUNT && count < MAX_MATCHES; i++)
            if (strncmp(commands[i].name, prefix, strlen(prefix)) == 0) {
                is_dir[count] = false;
                names[count++] = commands[i].name;
            }
    } else {
        /* "dir/qism" -> papka "dir/" va prefiks "qism" */
        char dir[VFS_PATH_MAX];
        const char *slash = 0;
        for (const char *p = word; *p; p++)
            if (*p == '/')
                slash = p;
        if (slash) {
            int dl = slash - word + 1;
            strncpy(dir, word, dl);
            dir[dl] = 0;
            prefix = slash + 1;
        } else {
            strcpy(dir, ".");
            prefix = word;
        }
        vfs_node_t *d;
        if (vfs_lookup(dir, &d) != VFS_OK || d->type != VFS_DIR)
            return;
        vfs_node_t *c;
        for (uint32_t i = 0; (c = vfs_readdir(d, i)) && count < MAX_MATCHES; i++)
            if (strncmp(c->name, prefix, strlen(prefix)) == 0) {
                is_dir[count] = (c->type == VFS_DIR);
                names[count++] = c->name;
            }
    }

    if (count == 0)
        return;

    /* Barcha variantlarning umumiy boshlanishi */
    int plen = strlen(prefix);
    int common = strlen(names[0]);
    for (int i = 1; i < count; i++) {
        int j = 0;
        while (j < common && names[i][j] == names[0][j])
            j++;
        common = j;
    }

    for (int j = plen; j < common && *len < LINE_MAX - 2; j++) {
        buf[(*len)++] = names[0][j];
        vga_putc(names[0][j]);
    }

    if (count == 1) {                           /* yagona variant: "/" yoki bo'sh joy qo'shamiz */
        char tail = is_dir[0] ? '/' : ' ';
        if (*len < LINE_MAX - 2) {
            buf[(*len)++] = tail;
            vga_putc(tail);
        }
    } else if (common == plen) {                /* umumiy qism yo'q: variantlarni ko'rsatamiz */
        kprintf("\n");
        for (int i = 0; i < count; i++) {
            if (is_dir[i])
                vga_set_color(VGA_LIGHT_BLUE, VGA_BLACK);
            kprintf("%s%s  ", names[i], is_dir[i] ? "/" : "");
            normal();
        }
        kprintf("\n");
        buf[*len] = 0;
        show_line_again(buf);
    }
    buf[*len] = 0;
}

/* ------------------------- Dasturlarni ishga tushirish ------------------------- */

/*
 * "hello abc"     -> /bin/hello ni ishga tushirib, tugashini kutish
 * "primes 1000 &" -> fonda ishga tushirish, shell darhol bo'shaydi
 * "/tmp/dastur"   -> to'liq yo'l bilan
 */
static bool run_program(const char *cmd, char *args)
{
    char path[VFS_PATH_MAX];
    bool has_slash = false;
    for (const char *p = cmd; *p; p++)
        if (*p == '/')
            has_slash = true;
    if (has_slash)
        strncpy(path, cmd, sizeof(path) - 1);
    else
        ksnprintf(path, sizeof(path), "/bin/%s", cmd);
    path[sizeof(path) - 1] = 0;

    vfs_node_t *n;
    if (vfs_lookup(path, &n) != VFS_OK || n->type != VFS_FILE)
        return false;

    /* Oxirida "&" bo'lsa — fonda */
    bool background = false;
    int len = strlen(args);
    while (len > 0 && args[len - 1] == ' ')
        args[--len] = 0;
    if (len > 0 && args[len - 1] == '&') {
        background = true;
        args[--len] = 0;
        while (len > 0 && args[len - 1] == ' ')
            args[--len] = 0;
    }

    task_reap();
    int32_t pid = process_spawn(path, args);
    if (pid < 0) {
        vga_set_color(VGA_LIGHT_RED, VGA_BLACK);
        kprintf("%s: %s\n", cmd, process_strerror(pid));
        normal();
        return true;
    }

    if (background) {
        kprintf("[PID %d] %s fonda ishga tushdi\n", pid, cmd);
        return true;
    }

    /* Oldingi plan: shell dastur tugashini kutadi (BLOCKED). Ctrl+C uni to'xtatadi */
    task_set_foreground((uint32_t)pid);
    int32_t code = task_wait((uint32_t)pid);
    task_set_foreground(0);
    task_reap_pid((uint32_t)pid);
    normal();

    if (code == EXIT_CTRL_C) {
        kprintf("^C\n");
    } else if (code != 0 && code != EXIT_FAULT) {
        vga_set_color(VGA_DARK_GREY, VGA_BLACK);
        kprintf("[%s %d kodi bilan tugadi]\n", cmd, code);
        normal();
    }
    return true;
}

/* Fonda ishlagan dasturlar tugagan bo'lsa — xabar berib, xotirasini bo'shatish */
static void report_finished(void)
{
    for (int i = 0; i < MAX_TASKS; i++) {
        task_t *t = task_get(i);
        if (t->state != TASK_ZOMBIE || !t->user || t->pid == task_foreground())
            continue;
        vga_set_color(VGA_DARK_GREY, VGA_BLACK);
        kprintf("[PID %u] %s tugadi (kod %d)\n", t->pid, t->name, t->exit_code);
        normal();
        task_reap_pid(t->pid);
    }
}

/* Qatorni buyruq nomi va argumentlarga ajratib, mos funksiyani chaqirish */
static void execute(char *line)
{
    char *cmd = (char *)skip_spaces(line);
    if (!*cmd)
        return;

    char *args = cmd;
    while (*args && *args != ' ')
        args++;
    if (*args) {
        *args = 0;
        args = (char *)skip_spaces(args + 1);
    }

    for (unsigned i = 0; i < COMMAND_COUNT; i++) {
        if (strcmp(cmd, commands[i].name) == 0) {
            commands[i].fn(args);
            return;
        }
    }

    if (run_program(cmd, args))      /* ichki buyruq emas — /bin dagi dasturmi? */
        return;

    vga_set_color(VGA_LIGHT_RED, VGA_BLACK);
    kprintf("Noma'lum buyruq: %s", cmd);
    normal();
    kprintf("  ('help' ni yozing)\n");
}

void shell_run(void)
{
    char line[LINE_MAX];
    char copy[LINE_MAX];

    char motd[256];
    int fd = vfs_open("/etc/motd", VFS_O_READ);       /* xush kelibsiz xabari fayldan */
    if (fd >= 0) {
        int32_t n = vfs_read(fd, motd, sizeof(motd) - 1);
        vfs_close(fd);
        if (n > 0) {
            motd[n] = 0;
            kprintf("%s", motd);
        }
    }
    kprintf("Buyruqlar ro'yxati uchun 'help' ni yozing.\n\n");

    for (;;) {
        report_finished();
        prompt();
        read_line(line);
        history_add(line);
        strcpy(copy, line);       /* execute satrni o'zgartiradi, tarix buzilmasin */
        execute(copy);
    }
}
