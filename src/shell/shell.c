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
#include "klog.h"
#include "irq.h"
#include "memory.h"
#include "gui.h"
#include "pci.h"

#define LINE_MAX    256
#define HISTORY_MAX 32

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
    uint32_t v = 0, limit = neg ? 0x80000000u : 0x7FFFFFFFu;
    while (*s >= '0' && *s <= '9') {
        uint32_t d = (uint32_t)(*s++ - '0');
        if (v > (limit - d) / 10)
            return false;               /* 32 bitga sig'maydigan son */
        v = v * 10 + d;
    }
    *out = neg ? (int32_t)(0u - v) : (int32_t)v;
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

/*
 * QATOR TAHRIRI (v0.6.1)
 * Kursorni qator ichida yurgizish va o'rtasiga yozish uchun qatorning ekrandagi
 * boshlanish joyini bilish kerak. Ekran katakchalarini 0 dan boshlab sanaymiz:
 * joy = qator * 80 + ustun ("chiziqli" koordinata). Shunda uzun qator keyingi
 * satrga o'tib ketsa ham hisob oddiy qo'shish bo'lib qoladi:
 *     kursor joyi = origin + pos
 * Qatorni qayta chizganda ekran pastga yetib suriladigan (scroll) bo'lsa,
 * origin ham shuncha qatorga yuqoriga suriladi.
 */
typedef struct {
    char *buf;
    int   len;          /* qatordagi belgilar soni */
    int   pos;          /* kursor: 0..len */
    int   origin;       /* birinchi belgining ekrandagi chiziqli joyi */
} line_t;

static int cursor_linear(void)
{
    int x, y;
    vga_get_cursor(&x, &y);
    return y * VGA_WIDTH + x;
}

static void cursor_to(int linear)
{
    vga_set_cursor(linear % VGA_WIDTH, linear / VGA_WIDTH);
}

/* Qatorni qayta chizish. old_len — avvalgi uzunlik (qisqargan bo'lsa, ortig'i o'chiriladi) */
static void line_redraw(line_t *l, int old_len)
{
    cursor_to(l->origin);
    int printed = 0;
    for (; printed < l->len; printed++)
        vga_putc_local(l->buf[printed]);
    for (; printed < old_len; printed++)
        vga_putc_local(' ');
    /* Ekran surilgan bo'lsa, kursor kutilganidan yuqorida turadi */
    l->origin -= l->origin + printed - cursor_linear();
    cursor_to(l->origin + l->pos);
}

static void line_set(line_t *l, const char *text)
{
    int old = l->len;
    strlcpy(l->buf, text, LINE_MAX);
    l->len = l->pos = (int)strlen(l->buf);
    line_redraw(l, old);
}

/*
 * Klaviaturadan bitta qator o'qish.
 *   chap/o'ng strelka, Home/End (Ctrl+A / Ctrl+E)  kursorni yurgizish
 *   Backspace, Delete (Ctrl+D)                    belgini o'chirish
 *   Ctrl+K / Ctrl+U                               kursordan oxirigacha / butun qatorni o'chirish
 *   yuqori/pastki strelka                         buyruqlar tarixi
 *   Tab                                           to'ldirish
 *   Ctrl+L                                        ekranni tozalash
 *   Ctrl+C                                        qatorni bekor qilish
 */
static void read_line(char *buf)
{
    line_t l = { buf, 0, 0, cursor_linear() };
    int hpos = history_count;
    buf[0] = 0;

    for (;;) {
        char c = keyboard_getchar();

        bool scroll_key = (c == KEY_PGUP || c == KEY_PGDN ||
                           c == KEY_SCROLL_UP || c == KEY_SCROLL_DOWN);
        if (!scroll_key && vga_view_offset()) {   /* boshqa tugma: jonli ekranga qaytamiz */
            vga_scroll_view(-vga_view_offset());
            status_bar_refresh();
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

        int old = l.len;
        switch (c) {
        case '\n':
            cursor_to(l.origin + l.len);
            vga_putc_local('\n');
            buf[l.len] = 0;
            serial_puts(buf);               /* tahrirlash ekranda bo'ldi; serialga tayyor qator */
            serial_putc('\n');
            return;

        case '\b':
            if (l.pos > 0) {
                memmove(buf + l.pos - 1, buf + l.pos, (size_t)(l.len - l.pos));
                l.pos--;
                l.len--;
                line_redraw(&l, old);
            }
            break;

        case KEY_DELETE:
            if (l.pos < l.len) {
                memmove(buf + l.pos, buf + l.pos + 1, (size_t)(l.len - l.pos - 1));
                l.len--;
                line_redraw(&l, old);
            }
            break;

        case KEY_LEFT:
            if (l.pos > 0)
                cursor_to(l.origin + --l.pos);
            break;

        case KEY_RIGHT:
            if (l.pos < l.len)
                cursor_to(l.origin + ++l.pos);
            break;

        case KEY_HOME:
            l.pos = 0;
            cursor_to(l.origin);
            break;

        case KEY_END:
            l.pos = l.len;
            cursor_to(l.origin + l.pos);
            break;

        case KEY_CTRL_K:                    /* kursordan oxirigacha o'chirish */
            l.len = l.pos;
            line_redraw(&l, old);
            break;

        case KEY_CTRL_U:                    /* butun qatorni o'chirish */
            l.len = l.pos = 0;
            line_redraw(&l, old);
            break;

        case KEY_CTRL_L:                    /* ekranni tozalab, qatorni qayta ko'rsatish */
            vga_clear();
            prompt();
            l.origin = cursor_linear();
            line_redraw(&l, 0);
            break;

        case KEY_UP:
        case KEY_DOWN:
            if (c == KEY_UP && hpos > 0)
                hpos--;
            else if (c == KEY_DOWN && hpos < history_count)
                hpos++;
            else
                break;
            line_set(&l, hpos < history_count ? history[hpos] : "");
            break;

        case KEY_CTRL_C:                    /* qatorni bekor qilish */
            cursor_to(l.origin + l.len);
            kprintf("^C\n");
            l.len = l.pos = 0;
            buf[0] = 0;
            hpos = history_count;
            prompt();
            l.origin = cursor_linear();
            break;

        case '\t':
            /* To'ldirish qator oxirida ishlaydi: kursorni o'sha yerga olib boramiz */
            l.pos = l.len;
            cursor_to(l.origin + l.len);
            buf[l.len] = 0;
            tab_complete(buf, &l.len);
            l.pos = l.len;
            l.origin = cursor_linear() - l.len;   /* variantlar chiqqan bo'lsa, qator pastga ko'chgan */
            break;

        default:
            if (c >= 32 && c < 127 && l.len < LINE_MAX - 1) {
                memmove(buf + l.pos + 1, buf + l.pos, (size_t)(l.len - l.pos));
                buf[l.pos++] = c;
                l.len++;
                line_redraw(&l, old);
            }
            break;
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

    /* Natija 32 bitga sig'ishini 64-bitli oraliq qiymat bilan tekshiramiz.
       (-2147483648 / -1 protsessorda exception beradi — kalkulyator kernelni qulatmasligi kerak) */
    int64_t r;
    switch (op) {
    case '+': r = (int64_t)a + b; break;
    case '-': r = (int64_t)a - b; break;
    case '*': r = (int64_t)a * b; break;
    case '/':
    case '%':
        if (b == 0) {
            vga_set_color(VGA_LIGHT_RED, VGA_BLACK);
            kprintf("Xato: 0 ga bo'lib bo'lmaydi\n");
            normal();
            return;
        }
        if (b == -1)
            r = (op == '/') ? -(int64_t)a : 0;
        else
            r = (op == '/') ? a / b : a % b;
        break;
    default:
        goto usage;
    }
    if (r > 0x7FFFFFFFLL || r < -0x80000000LL) {
        vga_set_color(VGA_LIGHT_RED, VGA_BLACK);
        kprintf("Xato: natija 32 bitli songa sig'maydi\n");
        normal();
        return;
    }
    kprintf("= %d\n", (int32_t)r);
    return;
usage:
    kprintf("Foydalanish: calc <son> <+ - * / %%> <son>   masalan: calc 12 * 7\n");
}

static void cmd_color(const char *args)
{
    const char *p = args;
    int32_t c;
    if (!parse_int(&p, &c) || c < 1 || c > 15) {
        kprintf("Foydalanish: color <1-15>\n");
        for (uint8_t i = 1; i < 16; i++) {
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

/* Cheksiz rekursiya: har chaqiruvda stekdan ~512 bayt oladi (ataylab — sinov uchun) */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpragmas"
#pragma GCC diagnostic ignored "-Winfinite-recursion"
static volatile bool eat_yield;
static uint32_t __attribute__((noinline)) eat_stack(uint32_t depth)
{
    volatile char pad[480];
    pad[0] = (char)depth;
    if (eat_yield)
        task_yield();                    /* scheduler stekni tekshirib tursin */
    return eat_stack(depth + 1) + (uint32_t)pad[0];
}
#pragma GCC diagnostic pop

static void eat_task(uint32_t arg)
{
    eat_yield = true;
    eat_stack(arg);
}

/*
 * Ataylab kernelni qulatish — himoya mexanizmlari qanday ishlashini ko'rsatadi:
 *   crash         0 ga bo'lish (exception #0)
 *   crash stack   cheksiz rekursiya: stek qo'riqchi sahifaga yetadi -> Double Fault
 *   crash canary  fon oqimida rekursiya: scheduler stek tubidagi belgini tekshirib ushlaydi
 *   crash heap    ajratilgan blokdan tashqariga yozish: heap tekshiruvi ushlaydi
 */
static void cmd_crash(const char *args)
{
    if (strcmp(args, "stack") == 0) {
        kprintf("Cheksiz rekursiya... (Double Fault kutilmoqda)\n");
        eat_yield = false;
        eat_stack(1);
        return;
    }
    if (strcmp(args, "canary") == 0) {
        kprintf("Fon oqimida rekursiya... (stek qo'riqchisi kutilmoqda)\n");
        task_create("stackeat", eat_task, 1);
        return;
    }
    if (strcmp(args, "heap") == 0) {
        kprintf("16 baytli blokka 64 bayt yozish... (heap tekshiruvi kutilmoqda)\n");
        volatile char *p = kmalloc(16);
        for (int i = 0; i < 64; i++)
            p[i] = 'X';                  /* qo'shni blok sarlavhasi buziladi */
        kfree(kmalloc(8));               /* keyingi kmalloc ro'yxat bo'ylab yurib, buzilishni topadi */
        return;
    }
    kprintf("0 ga bo'lish sinovi... (Exception #0 kutilmoqda)\n");
    volatile int a = 1, b = 0;
    a = a / b;
}

/* Grafik rejimga o'tish (v0.7) */
static void cmd_gui(const char *args)
{
    (void)args;
    const char *err = gui_start();
    if (err) {
        vga_set_color(VGA_LIGHT_RED, VGA_BLACK);
        kprintf("gui: %s\n", err);
        normal();
        return;
    }
    kprintf("Grafik rejim: 1024x768, 32 bit. Shell endi shu terminal oynasida.\n");
    kprintf("Start menyusi: pastki chap burchakdagi 'DiyorOS' tugmasi.\n");
}

/* Sichqoncha tezligi (grafik rejim uchun) */
static void cmd_mouse(const char *args)
{
    int32_t v;
    const char *p = args;
    if (*args && parse_int(&p, &v) && v >= 1 && v <= 10) {
        gui_set_mouse_speed((int)v);
        kprintf("Sichqoncha tezligi: %d (1 - eng sekin, 10 - eng tez)\n", gui_mouse_speed());
        return;
    }
    if (*args) {
        kprintf("Foydalanish: mouse <1-10>   (5 - standart)\n");
        return;
    }
    kprintf("Sichqoncha tezligi: %d. O'zgartirish: mouse <1-10>\n", gui_mouse_speed());
}

/* PCI qurilmalari ro'yxati (Linux dagi lspci) */
static void cmd_pci(const char *args)
{
    (void)args;
    vga_set_color(VGA_LIGHT_CYAN, VGA_BLACK);
    kprintf("JOYI     VENDOR:QURILMA  ISHLAB CHIQARUVCHI  TURI\n");
    normal();
    for (int i = 0; i < pci_count(); i++) {
        const pci_device_t *d = pci_get(i);
        kprintf("%02x:%02x.%u  %04x:%04x       %-18s  %s\n", d->bus, d->slot, d->func,
                d->vendor, d->device, pci_vendor_name(d->vendor),
                pci_class_name(d->class_code, d->subclass));
    }
    kprintf("Jami: %d ta qurilma\n", pci_count());
}

/* Kernel jurnali: yuklanishdan beri nima bo'lgani (klog.c) */
static void cmd_dmesg(const char *args)
{
    (void)args;
    char chunk[128];
    uint32_t off = 0, n;
    while ((n = klog_read(off, chunk, sizeof(chunk))) > 0) {
        for (uint32_t i = 0; i < n; i++)
            vga_putc_local(chunk[i]);        /* jurnal serial portga allaqachon chiqqan */
        off += n;
    }
}

static void cmd_reboot(const char *args)
{
    (void)args;
    kprintf("Qayta yuklanmoqda...\n");
    power_reboot();
}

static void cmd_shutdown(const char *args)
{
    (void)args;
    kprintf("Tizim o'chirilmoqda...\n");
    power_off();
    kprintf("Kompyuterni endi o'chirishingiz mumkin.\n");
    for (;;)
        __asm__ volatile ("hlt");
}

/* Buyruqlar jadvali */
typedef enum { CAT_GENERAL, CAT_FILES, CAT_DISK, CAT_MEMORY, CAT_TASKS, CAT_SYSTEM, CAT_COUNT } category_t;

static const char *category_names[CAT_COUNT] = {
    "Umumiy", "Fayllar (v0.4)", "Disk (v0.6)", "Xotira (v0.2)", "Vazifalar (v0.3)", "Tizim va sinov"
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
    { "df",        CAT_FILES,   "Fayl tizimlari bandligi",                         cmd_df        },

    { "disk",      CAT_DISK,    "Disk va FAT32 haqida ma'lumot",                   cmd_disk      },
    { "mkfs",      CAT_DISK,    "Diskni FAT32 da formatlash: mkfs [nom]",          cmd_mkfs      },
    { "mount",     CAT_DISK,    "Ulangan fayl tizimlari | mount disk",             cmd_mount     },
    { "umount",    CAT_DISK,    "Fayl tizimini uzish: umount /disk",               cmd_umount    },

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
    { "sinx",      CAT_TASKS,   "Sinxronizatsiya: sinx poyga|ishlab|deadlock|faylasuflar", cmd_sinx },

    { "gui",       CAT_SYSTEM,  "Grafik rejim: oynalar, sichqoncha, dasturlar",    cmd_gui       },
    { "mouse",     CAT_SYSTEM,  "Sichqoncha tezligi: mouse <1-10>",                cmd_mouse     },
    { "pci",       CAT_SYSTEM,  "PCI qurilmalari ro'yxati",                        cmd_pci       },
    { "dmesg",     CAT_SYSTEM,  "Kernel jurnali (yuklanishdan beri xabarlar)",     cmd_dmesg     },
    { "crash",     CAT_SYSTEM,  "Panic sinovi: crash | stack | canary | heap",    cmd_crash     },
    { "pagefault", CAT_SYSTEM,  "Page Fault sinovi: pagefault | null | kod",       cmd_pagefault },
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
    vfs_lock();
    if (vfs_lookup("/bin", &bin) == VFS_OK) {
        int col = 1;
        for (uint32_t i = 0; (c = vfs_readdir(bin, i)); i++) {
            int w = (int)strlen(c->name) + 1;
            if (col + w > 78) {              /* 80 ustunga sig'masa — yangi qator */
                kprintf("\n ");
                col = 1;
            }
            kprintf(" %s", c->name);
            col += w;
        }
    }
    vfs_unlock();
    normal();
    kprintf("\nBatafsil: help <buyruq>.  Dasturni fonda ishlatish: nom &   To'xtatish: Ctrl+C\n");
    kprintf("Kanal: ls | grep o | wc    Faylga: sanoq 9 > f.txt (>> qo'shadi)    wc < f.txt\n");
    kprintf("Tab - to'ldirish, strelkalar - tarix va qator tahriri, g'ildirak - varaqlash.\n");
}

/* --------------------------- Tab bilan to'ldirish --------------------------- */

#define MAX_MATCHES 48

static void show_line_again(const char *buf)
{
    prompt();
    while (*buf)
        vga_putc_local(*buf++);
}

/*
 * Birinchi so'z — buyruq nomi, keyingilari — fayl yo'li.
 * Bitta mos kelsa — to'liq yoziladi; bir nechta bo'lsa — umumiy qismi
 * yoziladi, umumiy qism bo'lmasa — variantlar ro'yxati chiqariladi.
 */
static void tab_complete_locked(char *buf, int *len)
{
    int start = *len;
    while (start > 0 && !(buf[start - 1] == ' ' && (start < 2 || buf[start - 2] != '\\')))
        start--;                                /* "\ " — so'zning bir qismi */
    bool is_command = true;
    for (int i = 0; i < start; i++)
        if (buf[i] != ' ')
            is_command = false;

    char word_buf[LINE_MAX];                    /* "\ " larni oddiy bo'sh joyga aylantiramiz */
    int wl = 0;
    for (int i = start; i < *len; i++) {
        if (buf[i] == '\\' && i + 1 < *len)
            i++;
        word_buf[wl++] = buf[i];
    }
    word_buf[wl] = 0;
    const char *word = word_buf;
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

    for (int j = plen; j < common && *len < LINE_MAX - 3; j++) {
        if (names[0][j] == ' ') {               /* bo'sh joy -> "\ " */
            buf[(*len)++] = '\\';
            vga_putc_local('\\');
        }
        buf[(*len)++] = names[0][j];
        vga_putc_local(names[0][j]);
    }

    if (count == 1) {                           /* yagona variant: "/" yoki bo'sh joy qo'shamiz */
        char tail = is_dir[0] ? '/' : ' ';
        if (*len < LINE_MAX - 2) {
            buf[(*len)++] = tail;
            vga_putc_local(tail);
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

/* Variantlar — tugun nomlariga ko'rsatkichlar: ular bilan ishlab bo'lguncha VFS qulfi
   ushlanadi (boshqa vazifa faylni o'chirib yubormasin) */
static void tab_complete(char *buf, int *len)
{
    vfs_lock();
    tab_complete_locked(buf, len);
    vfs_unlock();
}

/* ------------------------- Dasturlarni ishga tushirish ------------------------- */

/*
 * "hello abc"     -> /bin/hello ni ishga tushirib, tugashini kutish
 * "primes 1000 &" -> fonda ishga tushirish, shell darhol bo'shaydi
 * "/tmp/dastur"   -> to'liq yo'l bilan
 */
/* Buyruq nomidan dastur yo'li: "hello" -> "/bin/hello". Fayl bo'lmasa — false */
static bool program_path(const char *cmd, char *path, uint32_t size)
{
    bool has_slash = false;
    for (const char *p = cmd; *p; p++)
        if (*p == '/')
            has_slash = true;
    if (has_slash)
        strncpy(path, cmd, size - 1);
    else
        ksnprintf(path, size, "/bin/%s", cmd);
    path[size - 1] = 0;

    vfs_node_t *n;
    return vfs_lookup(path, &n) == VFS_OK && n->type == VFS_FILE;
}

static void report_exit(const char *cmd, int32_t code)
{
    if (code == EXIT_CTRL_C) {
        kprintf("^C\n");
    } else if (code != 0 && code != EXIT_FAULT && code != EXIT_PIPE) {
        vga_set_color(VGA_DARK_GREY, VGA_BLACK);
        kprintf("[%s %d kodi bilan tugadi]\n", cmd, code);
        normal();
    }
}

static bool run_program(const char *cmd, char *args)
{
    char path[VFS_PATH_MAX];
    if (!program_path(cmd, path, sizeof(path)))
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
    /* "Oldingi plan" belgisi vazifa yaratilayotganda qo'yiladi (bo'linmas): aks holda
       dastur belgilanmasdan ishlab ketib, klaviaturadan o'qiy olmay qolishi mumkin edi */
    spawn_opts_t opts = { .fd_in = -1, .fd_out = -1, .pgid = 0, .foreground = !background };
    int32_t pid = process_spawn_ex(path, args, &opts);
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
    int32_t code = task_wait((uint32_t)pid);
    task_set_foreground(0);
    task_reap_pid((uint32_t)pid);
    normal();
    report_exit(cmd, code);
    return true;
}

/* ------------------------------ Kanallar: a | b | c ------------------------------ */
/*
 * "ls /bin | grep o | wc" — har bir bosqichning chiqishi keyingisining kirishiga.
 *
 * 1. Qator "|" bo'yicha bo'linadi (ko'pi bilan 4 bosqich), N-1 ta kanal yaratiladi.
 * 2. Dasturlar (2-, 3-... bosqich va 1-bosqich dastur bo'lsa) BIRDANIGA ishga
 *    tushiriladi — ular parallel ishlaydi: biri yozayotganda keyingisi o'qiydi.
 *    Hammasi bitta JARAYON GURUHIda (pgid = birinchi dastur PID) — Ctrl+C butun
 *    guruhni to'xtatadi, klaviatura guruhga tegishli.
 * 3. 1-bosqich shell ning ICHKI buyrug'i bo'lsa (ls, cat, ps, echo ...), u shell
 *    ichida bajariladi, ekranga chiqishi esa ushlab olinadi (vga_set_capture) va
 *    kanalga yoziladi. Chiqish avval xotiraga yig'iladi: ichki buyruq VFS qulfini
 *    ushlab turib ekranga yozsa ham kanal to'lib "qotib" qolmaydi (deadlock yo'q).
 * 4. Shell o'zidagi kanal uchlarini yopadi — aks holda wc hech qachon EOF ni
 *    ko'rmasdi (yozish uchi shell da ochiq qolardi). Keyin hammasini kutadi.
 *
 * Yo'naltirish (redirect) ham shu yerda: "sanoq 100 > /disk/son.txt",
 * "ls >> log", "wc < fayl". Fayl ochiladi va uning deskriptori dasturning
 * stdout / stdin i qilib beriladi — dastur buni bilmaydi ham, u fd 1 ga yozadi.
 */
#define PIPE_STAGES  4
#define CAPTURE_MAX  (64 * 1024)

static task_t   *shell_self = 0;
static char     *capture_buf = 0;
static uint32_t  capture_len = 0;
static bool      capture_overflow = false;

static bool capture_char(char c)
{
    if (!sched_active() || task_current() != shell_self)
        return false;                        /* boshqa vazifalar ekranga yozaveradi */
    if (capture_len < CAPTURE_MAX)
        capture_buf[capture_len++] = c;
    else
        capture_overflow = true;
    return true;
}

static const command_t *find_command(const char *name);

static void pipe_error(const char *fmt, const char *arg)
{
    vga_set_color(VGA_LIGHT_RED, VGA_BLACK);
    kprintf(fmt, arg);
    normal();
}

/* "a b > fayl" -> args "a b", *path = "fayl". Topilmasa — 0 */
static char *cut_redirect(char *args, char sym, bool *append)
{
    char *r = args;
    while (*r && *r != sym)
        r++;
    if (!*r)
        return 0;
    *r++ = 0;
    *append = (sym == '>' && *r == '>');
    if (*append)
        r++;
    char *path = (char *)skip_spaces(r);
    char *end = path;
    while (*end && *end != ' ')
        end++;
    *end = 0;
    for (int n = strlen(args); n > 0 && args[n - 1] == ' '; n--)   /* argumentlar oxiridagi bo'sh joylar */
        args[n - 1] = 0;
    return path;
}

static void run_pipeline(char *line)
{
    char *stage[PIPE_STAGES], *args[PIPE_STAGES];
    int count = 0;
    char *p = line;
    for (;;) {
        char *bar = p;
        while (*bar && *bar != '|')
            bar++;
        bool last = !*bar;
        *bar = 0;
        if (count == PIPE_STAGES) {
            pipe_error("Kanalda ko'pi bilan %s ta buyruq bo'lishi mumkin\n", "4");
            return;
        }
        char *s = (char *)skip_spaces(p);
        size_t n = strlen(s);
        while (n > 0 && s[n - 1] == ' ')
            s[--n] = 0;
        if (!*s) {
            pipe_error("Kanalda bo'sh buyruq bor: '%s'\n", "a | | b");
            return;
        }
        if (s[n - 1] == '&') {
            pipe_error("%s: kanal (|) fonda (&) ishga tushirilmaydi\n", s);
            return;
        }
        char *a = s;
        while (*a && *a != ' ')
            a++;
        if (*a) {
            *a = 0;
            a = (char *)skip_spaces(a + 1);
        }
        stage[count] = s;
        args[count] = a;
        count++;
        if (last)
            break;
        p = bar + 1;
    }

    /* Bosqichlarni tekshirish: 1-si ichki buyruq yoki dastur, qolganlari — faqat dastur */
    char paths[PIPE_STAGES][VFS_PATH_MAX];
    const command_t *builtin = 0;
    for (int i = 0; i < count; i++) {
        if (program_path(stage[i], paths[i], sizeof(paths[i])))
            continue;
        if (i == 0 && (builtin = find_command(stage[0])) != 0)
            continue;
        if (find_command(stage[i]))
            pipe_error("%s: ichki buyruq kanal o'rtasida/oxirida bo'lolmaydi (stdin o'qimaydi)\n", stage[i]);
        else
            pipe_error("%s: bunday dastur yo'q (/bin ga qarang: ls /bin)\n", stage[i]);
        return;
    }
    /* Yo'naltirishlar: "<" faqat birinchi, ">" faqat oxirgi bosqichda */
    bool append = false, dummy;
    char *in_path = 0, *out_path = 0;
    for (int i = 0; i < count; i++) {
        char *ip = cut_redirect(args[i], '<', &dummy);
        char *op = cut_redirect(args[i], '>', &append);
        if ((ip && i != 0) || (op && i != count - 1)) {
            pipe_error("%s: '<' faqat birinchi, '>' faqat oxirgi buyruqda bo'ladi\n", stage[i]);
            return;
        }
        if ((ip && !*ip) || (op && !*op)) {
            pipe_error("%s: '<' / '>' dan keyin fayl nomi kerak\n", stage[i]);
            return;
        }
        if (ip) in_path = ip;
        if (op) out_path = op;
    }
    if (builtin && (strcmp(stage[0], "edit") == 0 || strcmp(stage[0], "gui") == 0 ||
                    strcmp(stage[0], "clear") == 0)) {
        pipe_error("%s: ekran bilan ishlaydigan buyruqni kanal/faylga yo'naltirib bo'lmaydi\n", stage[0]);
        return;
    }
    if (in_path && builtin) {
        pipe_error("%s: ichki buyruq '<' ni qo'llamaydi (klaviaturadan o'qimaydi)\n", stage[0]);
        return;
    }
    if (builtin && count == 1 && !out_path)
        return;

    int in_fd = -1, out_fd = -1;
    if (in_path && (in_fd = vfs_open(in_path, VFS_O_READ)) < 0) {
        vga_set_color(VGA_LIGHT_RED, VGA_BLACK);
        kprintf("%s: %s\n", in_path, vfs_strerror(in_fd));
        normal();
        return;
    }
    if (out_path) {
        uint32_t fl = VFS_O_WRITE | VFS_O_CREATE | (append ? VFS_O_APPEND : VFS_O_TRUNC);
        if ((out_fd = vfs_open(out_path, fl)) < 0) {
            vga_set_color(VGA_LIGHT_RED, VGA_BLACK);
            kprintf("%s: %s\n", out_path, vfs_strerror(out_fd));
            normal();
            if (in_fd >= 0)
                vfs_close(in_fd);
            return;
        }
    }

    int pr[PIPE_STAGES - 1], pw[PIPE_STAGES - 1];
    int pipes = 0;
    for (; pipes < count - 1; pipes++) {
        int fds[2];
        int err = vfs_pipe(fds);
        if (err) {
            pipe_error("pipe: %s\n", vfs_strerror(err));
            for (int j = 0; j < pipes; j++) {
                vfs_close(pr[j]);
                vfs_close(pw[j]);
            }
            if (in_fd >= 0)
                vfs_close(in_fd);
            if (out_fd >= 0)
                vfs_close(out_fd);
            return;
        }
        pr[pipes] = fds[0];
        pw[pipes] = fds[1];
    }

    task_reap();
    uint32_t me = task_current()->pid;
    int32_t pids[PIPE_STAGES] = { 0 };
    uint32_t pgid = 0;
    bool failed = false;
    for (int i = builtin ? 1 : 0; i < count && !failed; i++) {
        spawn_opts_t o = { .fd_in = -1, .fd_out = -1, .pgid = pgid, .foreground = true };
        if (i > 0)
            o.fd_in = vfs_dup(pr[i - 1], me);      /* nusxa: muvaffaqiyatda bolaga o'tadi */
        else if (in_fd >= 0)
            o.fd_in = vfs_dup(in_fd, me);
        if (i < count - 1)
            o.fd_out = vfs_dup(pw[i], me);
        else if (out_fd >= 0)
            o.fd_out = vfs_dup(out_fd, me);
        int32_t pid = (o.fd_in < -1 || o.fd_out < -1) ? VFS_EMFILE
                                                      : process_spawn_ex(paths[i], args[i], &o);
        if (pid < 0) {
            if (o.fd_in >= 0)
                vfs_close(o.fd_in);
            if (o.fd_out >= 0)
                vfs_close(o.fd_out);
            vga_set_color(VGA_LIGHT_RED, VGA_BLACK);
            kprintf("%s: %s\n", stage[i], process_strerror(pid));
            normal();
            failed = true;
            break;
        }
        pids[i] = pid;
        if (!pgid)
            pgid = (uint32_t)pid;
    }
    if (failed && pgid)
        task_signal_group(pgid, EXIT_KILLED);      /* yarim qurilgan kanalni to'xtatamiz */

    /* Shell dagi uchlarni yopamiz (ichki buyruq yozadigan birinchi kanaldan tashqari) */
    for (int j = 0; j < pipes; j++) {
        vfs_close(pr[j]);
        if (!(builtin && j == 0))
            vfs_close(pw[j]);
    }

    /* Ichki buyruq chiqishi: kanalga yoki (yolg'iz bo'lsa) faylga */
    int builtin_out = !builtin ? -1 : (count > 1 ? pw[0] : out_fd);
    if (builtin) {
        if (!failed) {
            capture_buf = kmalloc(CAPTURE_MAX);
            capture_len = 0;
            capture_overflow = false;
            if (capture_buf) {
                vga_set_capture(capture_char);
                builtin->fn(args[0]);
                vga_set_capture(0);
                normal();
                for (uint32_t off = 0; off < capture_len; ) {
                    int32_t n = vfs_write(builtin_out, capture_buf + off, capture_len - off);
                    if (n <= 0)
                        break;                     /* EPIPE: o'quvchi tugagan */
                    off += (uint32_t)n;
                }
                kfree(capture_buf);
                capture_buf = 0;
                if (capture_overflow)
                    pipe_error("%s: chiqish 64 KB dan katta - qolgani tashlab yuborildi\n", stage[0]);
            } else {
                pipe_error("%s: xotira yetmadi\n", stage[0]);
            }
        }
        if (count > 1)
            vfs_close(pw[0]);                      /* -> keyingi bosqich EOF ko'radi */
    }
    if (in_fd >= 0)
        vfs_close(in_fd);
    if (out_fd >= 0)
        vfs_close(out_fd);

    int32_t last_code = 0;
    bool interrupted = false;
    for (int i = 0; i < count; i++) {
        if (!pids[i])
            continue;
        int32_t code = task_wait((uint32_t)pids[i]);
        if (code == EXIT_CTRL_C)
            interrupted = true;
        last_code = code;
    }
    task_set_foreground(0);
    for (int i = 0; i < count; i++)
        if (pids[i])
            task_reap_pid((uint32_t)pids[i]);
    normal();
    if (interrupted)
        kprintf("^C\n");
    else if (!failed && pids[count - 1])
        report_exit(stage[count - 1], last_code);
}

/* Fonda ishlagan dasturlar tugagan bo'lsa — xabar berib, xotirasini bo'shatish */
static void report_finished(void)
{
    for (int i = 0; i < MAX_TASKS; i++) {
        task_t *t = task_get(i);
        if (t->state != TASK_ZOMBIE || !t->user || t->pid == task_foreground())
            continue;
        task_t *parent = task_find(t->parent_pid);
        if (parent && parent->user && parent->state != TASK_ZOMBIE)
            continue;                        /* v0.8: bu bolani otasi o'zi wait() bilan yig'adi */
        /* TTIN da xabar allaqachon chiqqan; KILLED — oynasi yopilgan grafik dastur */
        if (t->exit_code != EXIT_TTIN && t->exit_code != EXIT_KILLED) {
            vga_set_color(VGA_DARK_GREY, VGA_BLACK);
            kprintf("[PID %u] %s tugadi (kod %d)\n", t->pid, t->name, t->exit_code);
            normal();
        }
        task_reap_pid(t->pid);
    }
    task_reap();                             /* tugagan kernel oqimlari (masalan, oyna menejeri) */
}

static const command_t *find_command(const char *name)
{
    for (unsigned i = 0; i < COMMAND_COUNT; i++)
        if (strcmp(name, commands[i].name) == 0)
            return &commands[i];
    return 0;
}

/* Qatorni buyruq nomi va argumentlarga ajratib, mos funksiyani chaqirish */
static void execute(char *line)
{
    char *cmd = (char *)skip_spaces(line);
    if (!*cmd)
        return;
    /* v0.8: kanal (|) va yo'naltirish (<, >). "echo matn > fayl" — o'zining eski usuli */
    bool is_echo = strncmp(cmd, "echo", 4) == 0 && (cmd[4] == ' ' || !cmd[4]);
    for (char *c = cmd; *c; c++)
        if (*c == '|' || (!is_echo && (*c == '>' || *c == '<'))) {
            run_pipeline(cmd);
            return;
        }

    /* "pi&" -> "pi &": & oldidan bo'sh joy shart bo'lmasin (v0.7) */
    size_t n = strlen(cmd);
    while (n > 0 && cmd[n - 1] == ' ')
        cmd[--n] = 0;
    if (n > 1 && cmd[n - 1] == '&' && cmd[n - 2] != ' ' && n + 1 < LINE_MAX - (size_t)(cmd - line)) {
        cmd[n - 1] = ' ';
        cmd[n] = '&';
        cmd[n + 1] = 0;
    }

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

    shell_self = task_current();

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
