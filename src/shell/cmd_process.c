/*
 * cmd_process.c — vazifalar bilan bog'liq shell buyruqlari (v0.3).
 *
 *   ps                 — barcha vazifalar: holati, CPU ulushi
 *   spawn <nom> [N]    — fon vazifasini ishga tushirish
 *   kill <PID>         — vazifani to'xtatish
 *   sched              — scheduler statistikasi
 *
 * Demo vazifalar (spawn bilan ishga tushiriladi):
 *   spinner — status barda aylanuvchi belgi: shell bilan PARALLEL ishlashni ko'rsatadi
 *   primes  — N gacha tub sonlarni sanaydi va o'zi tugaydi
 *   busy    — cheksiz hisoblaydi: preemptive scheduler uni majburan to'xtatib,
 *             shell ga ham vaqt berishini ko'rsatadi
 */
#include <stdint.h>
#include <stdbool.h>
#include "shell.h"
#include "kernel.h"
#include "task.h"
#include "kprintf.h"
#include "vga.h"
#include "drivers.h"
#include "string.h"
#include "config.h"

/* ------------------------------ Yordamchilar ------------------------------ */

static const char *next_word(const char *s, char *out, int max)
{
    while (*s == ' ')
        s++;
    int i = 0;
    while (*s && *s != ' ' && i < max - 1)
        out[i++] = *s++;
    out[i] = 0;
    while (*s && *s != ' ')
        s++;
    return s;
}

static bool to_uint(const char *s, uint32_t *out)
{
    if (!*s)
        return false;
    uint32_t v = 0;
    for (; *s; s++) {
        if (*s < '0' || *s > '9')
            return false;
        v = v * 10 + (*s - '0');
    }
    *out = v;
    return true;
}

/* Raqamni satrga yozish (vazifalar o'z "note" maydonini to'ldirishi uchun) */
static char *put_uint(char *p, uint32_t v)
{
    char tmp[12];
    int n = 0;
    do {
        tmp[n++] = '0' + v % 10;
        v /= 10;
    } while (v);
    while (n)
        *p++ = tmp[--n];
    *p = 0;
    return p;
}

static char *put_str(char *p, const char *s)
{
    while (*s)
        *p++ = *s++;
    *p = 0;
    return p;
}

static bool task_alive(uint32_t pid)
{
    for (int i = 0; i < MAX_TASKS; i++) {
        task_t *t = task_get(i);
        if (t->pid == pid && t->state != TASK_UNUSED && t->state != TASK_ZOMBIE)
            return true;
    }
    return false;
}

/* ------------------------------ Demo vazifalar ----------------------------- */

#define SPINNER_SLOTS ((STATUS_TASK_END - STATUS_TASK_COL) / 2 + 1)   /* 7 ta */
static uint32_t spinner_owner[SPINNER_SLOTS];

/* O'lgan spinner larning belgilarini status bardan o'chirish */
static void spinner_cleanup(void)
{
    for (int i = 0; i < SPINNER_SLOTS; i++)
        if (spinner_owner[i] && !task_alive(spinner_owner[i])) {
            vga_write_at(STATUS_TASK_COL + i * 2, 0, " ", VGA_WHITE, VGA_BLUE);
            spinner_owner[i] = 0;
        }
}

static void spinner_task(uint32_t column)
{
    static const char frames[] = "|/-\\";
    static const uint8_t colors[] = { VGA_YELLOW, VGA_LIGHT_GREEN, VGA_LIGHT_CYAN,
                                      VGA_LIGHT_MAGENTA, VGA_WHITE, VGA_LIGHT_RED, VGA_YELLOW };
    uint8_t color = colors[(column - STATUS_TASK_COL) / 2 % 7];
    uint32_t speed = 60 + (column - STATUS_TASK_COL) * 15;   /* har biri o'z tezligida */
    char ch[2] = { 0, 0 };
    task_t *me = task_current();

    for (uint32_t i = 0;; i++) {
        ch[0] = frames[i % 4];
        vga_write_at(column, 0, ch, color, VGA_BLUE);
        char *p = put_str(me->note, "aylanish: ");
        put_uint(p, i / 4);
        timer_sleep_ms(speed);               /* uxlaydi — protsessor boshqalarga */
    }
}

static void primes_task(uint32_t limit)
{
    task_t *me = task_current();
    uint32_t count = 0;

    for (uint32_t n = 2; n <= limit; n++) {
        bool prime = true;
        for (uint32_t d = 2; d * d <= n; d++)
            if (n % d == 0) {
                prime = false;
                break;
            }
        if (prime)
            count++;
        if (n % 4096 == 0) {                 /* vaqti-vaqti bilan holatni yangilaymiz */
            char *p = put_str(me->note, "");
            p = put_uint(p, n / (limit / 100 + 1));   /* 64-bit bo'lishsiz foiz */
            p = put_str(p, "%, topildi: ");
            put_uint(p, count);
        }
    }

    char *p = put_str(me->note, "tayyor: ");
    p = put_uint(p, count);
    put_str(p, " ta tub son");
    /* funksiya qaytadi -> task_exit() -> vazifa tugaydi */
}

static void busy_task(uint32_t arg)
{
    (void)arg;
    task_t *me = task_current();
    volatile uint32_t counter = 0;
    for (;;) {                               /* hech qachon navbat bermaydi */
        counter++;
        if ((counter & 0xFFFFF) == 0) {
            char *p = put_uint(me->note, counter >> 20);
            put_str(p, " mln aylanish");
        }
    }
}

/* -------------------------------- Buyruqlar -------------------------------- */

static const char *state_name(task_state_t s)
{
    switch (s) {
    case TASK_READY:    return "tayyor";
    case TASK_RUNNING:  return "ishlayapti";
    case TASK_SLEEPING: return "uxlayapti";
    case TASK_BLOCKED:  return "kutyapti";
    case TASK_ZOMBIE:   return "tugagan";
    default:            return "?";
    }
}

static uint8_t state_color(task_state_t s)
{
    switch (s) {
    case TASK_RUNNING:  return VGA_LIGHT_GREEN;
    case TASK_READY:    return VGA_YELLOW;
    case TASK_ZOMBIE:   return VGA_DARK_GREY;
    default:            return VGA_LIGHT_CYAN;
    }
}

void cmd_ps(const char *args)
{
    (void)args;
    spinner_cleanup();
    uint8_t old = vga_get_color();

    vga_set_color(VGA_WHITE, VGA_BLACK);
    kprintf("PID  %-10s%-12s CPU%%   TICK  ALMASH  IZOH\n", "NOMI", "HOLAT");
    vga_set_color(old & 0x0F, old >> 4);

    int count = 0;
    for (int i = 0; i < MAX_TASKS; i++) {
        task_t *t = task_get(i);
        if (t->state == TASK_UNUSED)
            continue;
        count++;
        kprintf("%3u  %-10s", t->pid, t->name);
        vga_set_color(state_color(t->state), VGA_BLACK);
        kprintf("%-12s", state_name(t->state));
        vga_set_color(old & 0x0F, old >> 4);
        kprintf("%4u%% %6u %7u  %s\n", t->cpu_percent, t->cpu_ticks, t->switches, t->note);
    }
    kprintf("Jami: %d ta vazifa (maksimum %d)\n", count, MAX_TASKS);
}

void cmd_spawn(const char *args)
{
    char name[16], num[16];
    const char *rest = next_word(args, name, sizeof(name));
    next_word(rest, num, sizeof(num));

    task_reap();                             /* tugaganlarning xotirasini bo'shatamiz */
    spinner_cleanup();

    uint32_t pid = 0;
    if (strcmp(name, "spinner") == 0) {
        int slot = -1;
        for (int i = 0; i < SPINNER_SLOTS; i++)
            if (!spinner_owner[i]) {
                slot = i;
                break;
            }
        if (slot < 0) {
            kprintf("Status barda joy qolmadi (maksimum %d ta spinner)\n", SPINNER_SLOTS);
            return;
        }
        pid = task_create("spinner", spinner_task, STATUS_TASK_COL + slot * 2);
        if (pid)
            spinner_owner[slot] = pid;
    } else if (strcmp(name, "primes") == 0) {
        uint32_t limit = 10000000;
        if (num[0] && (!to_uint(num, &limit) || limit < 10)) {
            kprintf("Foydalanish: spawn primes [N]   masalan: spawn primes 1000000\n");
            return;
        }
        pid = task_create("primes", primes_task, limit);
    } else if (strcmp(name, "busy") == 0) {
        pid = task_create("busy", busy_task, 0);
    } else {
        kprintf("Foydalanish: spawn <spinner | primes [N] | busy>\n");
        kprintf("  spinner  status barda aylanuvchi belgi (fon vazifasi)\n");
        kprintf("  primes   N gacha tub sonlarni sanaydi va tugaydi\n");
        kprintf("  busy     cheksiz hisoblaydi (preemption sinovi)\n");
        return;
    }

    if (pid)
        kprintf("Ishga tushdi: %s, PID %u\n", name, pid);
    else
        kprintf("Vazifa yaratib bo'lmadi: jadval to'la yoki xotira yetmadi\n");
}

void cmd_kill(const char *args)
{
    char num[16];
    uint32_t pid;
    next_word(args, num, sizeof(num));
    if (!to_uint(num, &pid)) {
        kprintf("Foydalanish: kill <PID>   (PID larni 'ps' ko'rsatadi)\n");
        return;
    }
    if (pid == 0 || pid == 1) {
        kprintf("PID %u (%s) tizim vazifasi, uni to'xtatib bo'lmaydi\n",
                pid, pid == 0 ? "idle" : "shell");
        return;
    }
    if (task_kill(pid)) {
        spinner_cleanup();
        kprintf("PID %u to'xtatildi\n", pid);
    } else {
        kprintf("PID %u topilmadi\n", pid);
    }
}

void cmd_sched(const char *args)
{
    (void)args;
    int ready = 0, total = 0;
    for (int i = 0; i < MAX_TASKS; i++) {
        task_t *t = task_get(i);
        if (t->state == TASK_UNUSED)
            continue;
        total++;
        if (t->state == TASK_READY || t->state == TASK_RUNNING)
            ready++;
    }
    kprintf("Algoritm:             Round-Robin, preemptive (majburiy)\n");
    kprintf("Vaqt kvanti:          %u tick = %u ms\n", SCHED_QUANTUM, SCHED_QUANTUM * 1000 / TIMER_HZ);
    kprintf("Taymer chastotasi:    %u Hz\n", TIMER_HZ);
    kprintf("Vazifalar:            %d ta, ulardan ishlashga tayyor: %d\n", total, ready);
    kprintf("Kontekst almashishlar: %u marta (%u soniyada)\n", sched_switch_count(), timer_seconds());
    kprintf("Joriy vazifa:         PID %u (%s)\n", task_current()->pid, task_current()->name);
}
