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

#define LINE_MAX    128
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
    vga_set_color(VGA_LIGHT_GREEN, VGA_BLACK);
    kprintf("%s", OS_NAME);
    vga_set_color(VGA_WHITE, VGA_BLACK);
    kprintf("> ");
    normal();
}

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
typedef struct {
    const char *name;
    const char *description;
    void (*fn)(const char *args);
} command_t;

static const command_t commands[] = {
    /* Umumiy */
    { "help",      "Buyruqlar ro'yxati",                     cmd_help      },
    { "clear",     "Ekranni tozalash",                       cmd_clear     },
    { "echo",      "Matnni chiqarish: echo salom",           cmd_echo      },
    { "about",     "OS haqida ma'lumot",                     cmd_about     },
    { "whoami",    "Muallif haqida",                         cmd_whoami    },
    { "uptime",    "Tizim qancha vaqtdan beri ishlayapti",   cmd_uptime    },
    { "time",      "Sana va vaqt (CMOS soati)",              cmd_time      },
    { "calc",      "Kalkulyator: calc 12 * 7",               cmd_calc      },
    { "color",     "Matn rangini o'zgartirish: color 14",    cmd_color     },
    { "history",   "Kiritilgan buyruqlar tarixi",            cmd_history   },
    { "sleep",     "N soniya kutish: sleep 3",               cmd_sleep     },
    /* Xotira (v0.2) */
    { "mem",       "Xotira holati: RAM, paging, heap",       cmd_mem       },
    { "memmap",    "BIOS xotira xaritasi",                   cmd_memmap    },
    { "alloc",     "Heap dan xotira olish: alloc 100",       cmd_alloc     },
    { "free",      "Xotirani qaytarish: free 0xD0000018",    cmd_free      },
    { "heap",      "Heap bloklari ro'yxati",                 cmd_heap      },
    { "memtest",   "Heap ni avtomatik sinash",               cmd_memtest   },
    { "translate", "Virtual -> fizik: translate 0xD0000000", cmd_translate },
    /* Sinov va boshqaruv */
    { "crash",     "Exception sinovi (0 ga bo'lish)",        cmd_crash     },
    { "pagefault", "Page Fault sinovi (pagefault null)",     cmd_pagefault },
    { "reboot",    "Qayta yuklash",                          cmd_reboot    },
    { "shutdown",  "O'chirish",                              cmd_shutdown  },
};
#define COMMAND_COUNT (sizeof(commands) / sizeof(commands[0]))

static void cmd_help(const char *args)
{
    (void)args;
    kprintf("Mavjud buyruqlar:\n");
    for (unsigned i = 0; i < COMMAND_COUNT; i++) {
        vga_set_color(VGA_YELLOW, VGA_BLACK);
        kprintf("  %s", commands[i].name);
        for (int pad = strlen(commands[i].name); pad < 11; pad++)
            vga_putc(' ');
        normal();
        kprintf("%s\n", commands[i].description);
    }
    kprintf("Yuqori/pastki strelka: oldingi buyruqlar.\n");
    kprintf("Varaqlash: sichqoncha g'ildiragi, Shift+strelka, PgUp/PgDn (Mac: Fn+strelka).\n");
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

    vga_set_color(VGA_LIGHT_RED, VGA_BLACK);
    kprintf("Noma'lum buyruq: %s", cmd);
    normal();
    kprintf("  ('help' ni yozing)\n");
}

void shell_run(void)
{
    char line[LINE_MAX];
    char copy[LINE_MAX];

    kprintf("Buyruqlar ro'yxati uchun 'help' ni yozing.\n\n");

    for (;;) {
        prompt();
        read_line(line);
        history_add(line);
        strcpy(copy, line);       /* execute satrni o'zgartiradi, tarix buzilmasin */
        execute(copy);
    }
}
