/*
 * cmd_sync.c — "sinx" buyrug'i: sinxronizatsiya masalalari namoyishi (v0.8).
 *
 *   sinx               qisqa yordam va /proc/sync holati
 *   sinx poyga         race condition: qulfsiz counter++ va mutex bilan
 *   sinx ishlab        producer-consumer: semaforlar + mutex, 5 o'rinli bufer
 *   sinx deadlock      ikki oqim A->B va B->A tartibda qulf oladi: aniqlash va chiqish
 *   sinx faylasuflar   "ovqatlanayotgan faylasuflar" (Dijkstra): 5 ta aylana deadlock
 *   sinx faylasuflar tartib   resurslarni tartiblash bilan — deadlock umuman yo'q
 *
 * Har bir namoyish haqiqiy kernel oqimlarida ishlaydi: ularni taymer (preemptive
 * scheduler) istalgan joyda to'xtatib, boshqasiga o'tkazadi — muammolar
 * sun'iy emas, haqiqiy. Ekranga chiqarish ham "konsol" mutexi bilan: aks
 * holda ikki oqimning qatorlari bir-biriga aralashib ketardi.
 */
#include <stdint.h>
#include <stdbool.h>
#include <stdarg.h>
#include "shell.h"
#include "sync.h"
#include "task.h"
#include "kprintf.h"
#include "vga.h"
#include "drivers.h"
#include "string.h"
#include "config.h"
#include "irq.h"
#include "vfs.h"
#include "memory.h"

/* ------------------------------- Yordamchilar ------------------------------- */

static mutex_t console;

static void say(uint8_t color, const char *fmt, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    kvsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    mutex_lock(&console);                    /* butun qator bitta bo'lib chiqadi */
    uint8_t old = vga_get_color();
    vga_set_color(color, VGA_BLACK);
    kprintf("%s", buf);
    vga_set_color(old & 0x0F, old >> 4);
    mutex_unlock(&console);
}

static uint32_t rng = 12345;
static uint32_t rnd(uint32_t lo, uint32_t hi)    /* [lo, hi] */
{
    uint32_t f = irq_save();
    rng = rng * 1103515245u + 12345u;
    uint32_t v = (rng >> 16) % (hi - lo + 1) + lo;
    irq_restore(f);
    return v;
}

static uint32_t ms_since(uint32_t start)
{
    return (timer_ticks() - start) * (1000 / TIMER_HZ);
}

/* Oqimlarni ishga tushirib, hammasi tugashini kutish */
static void run_threads(const char *prefix, int count, task_entry_t fn)
{
    uint32_t pids[8];
    char name[TASK_NAME_LEN];
    for (int i = 0; i < count; i++) {
        ksnprintf(name, sizeof(name), "%s%d", prefix, i + 1);
        pids[i] = task_create(name, fn, (uint32_t)i);
    }
    for (int i = 0; i < count; i++)
        if (pids[i])
            task_wait(pids[i]);
    task_reap();
}

/* Joriy vazifa m ni so'raganda hosil bo'lgan kutish aylanasini matnga aylantirish:
   "dl2 -> printer[dl1] -> skaner[dl2]" — qulf nomi va [uni ushlab turgan oqim] */
static void describe_cycle(const mutex_t *m, char *b, uint32_t size)
{
    uint32_t f = irq_save();
    const task_t *me = task_current();
    int n = ksnprintf(b, size, "%s -> %s", me->name, m->name);
    const task_t *owner = m->owner;
    for (int i = 0; owner && i < MAX_TASKS && n < (int)size - 40; i++) {
        n += ksnprintf(b + n, size - n, "[%s]", owner->name);
        if (owner == me || !owner->waiting_mutex)
            break;
        n += ksnprintf(b + n, size - n, " -> %s", owner->waiting_mutex->name);
        owner = owner->waiting_mutex->owner;
    }
    irq_restore(f);
}

/* ------------------------------- 1. Poyga ------------------------------- */

#define RACE_THREADS 3
#define RACE_N       30000

static volatile uint32_t counter;
static volatile bool     race_locked;
static mutex_t           race_mutex;

static void work(void)
{
    for (volatile int i = 0; i < 1000; i++)   /* "hisoblash" — o'qish va yozish orasidagi vaqt */
        ;
}

static void race_thread(uint32_t id)
{
    (void)id;
    for (int i = 0; i < RACE_N; i++) {
        if (race_locked)
            mutex_lock(&race_mutex);
        uint32_t v = counter;                /* 1) o'qish */
        work();                              /* 2) hisoblash: shu payt taymer bizni to'xtatishi mumkin */
        counter = v + 1;                     /* 3) yozish: boshqalarning o'zgarishi ustidan! */
        if (race_locked)
            mutex_unlock(&race_mutex);
    }
}

static void demo_race(void)
{
    const uint32_t expected = RACE_THREADS * RACE_N;
    say(VGA_WHITE, "POYGA (race condition): %d oqim, har biri %d marta counter++\n", RACE_THREADS, RACE_N);
    say(VGA_LIGHT_GREY, "  counter++ = o'qish -> hisoblash -> yozish. Uning o'rtasida taymer oqimni\n"
                        "  almashtirsa, boshqa oqimning qo'shganlari ustidan yozib yuboriladi.\n\n");

    for (int round = 0; round < 2; round++) {
        counter = 0;
        race_locked = round == 1;
        mutex_init(&race_mutex, "poyga", false);
        uint32_t t0 = timer_ticks();
        run_threads("poyga", RACE_THREADS, race_thread);
        uint32_t ms = ms_since(t0);
        uint32_t got = counter;

        say(VGA_YELLOW, "%d) %s\n", round + 1, round ? "Mutex bilan (kritik seksiya himoyalangan)"
                                                     : "Qulfsiz (himoyasiz umumiy o'zgaruvchi)");
        if (got == expected)
            say(VGA_LIGHT_GREEN, "   natija %u, kutilgan %u -> TO'G'RI", got, expected);
        else
            say(VGA_LIGHT_RED, "   natija %u, kutilgan %u -> %u ta qo'shish YO'QOLDI", got, expected, expected - got);
        say(VGA_LIGHT_GREY, "   (%u ms)\n", ms);
        if (round)
            say(VGA_LIGHT_GREY, "   qulf %u marta olindi, %u marta band edi - oqim uxlab navbat kutdi\n",
                race_mutex.acquisitions, race_mutex.contentions);
    }
    say(VGA_LIGHT_GREY, "\nXulosa: umumiy ma'lumotni o'zgartiradigan kod - kritik seksiya. Unga bir\n"
                        "paytda faqat bitta oqim kirishi kerak (o'zaro istisno = mutex).\n");
}

/* -------------------------- 2. Ishlab chiqaruvchi - iste'molchi -------------------------- */

#define PC_SLOTS 5
#define PC_ITEMS 8                           /* har bir ishlab chiqaruvchi nechta mahsulot */

static int         pc_buf[PC_SLOTS];
static int         pc_in, pc_out, pc_count;
static int         pc_next_item;
static semaphore_t pc_empty, pc_full;        /* bo'sh o'rinlar / to'la o'rinlar */
static mutex_t     pc_mutex;                 /* bufer indekslarini himoya qiladi */
static uint32_t    pc_t0;

static void buffer_picture(char *b)
{
    b[0] = '[';
    for (int i = 0; i < PC_SLOTS; i++)
        b[1 + i] = i < pc_count ? '#' : '.';
    b[1 + PC_SLOTS] = ']';
    b[2 + PC_SLOTS] = 0;
}

static void producer(uint32_t id)
{
    char pic[PC_SLOTS + 3];
    for (int k = 0; k < PC_ITEMS; k++) {
        timer_sleep_ms(rnd(20, 70));         /* mahsulot tayyorlash */
        if (!sem_trydown(&pc_empty)) {
            say(VGA_DARK_GREY, "%5u ms  Ishlab-%u: bufer TO'LA - kutadi (sem_down bo'sh)\n",
                ms_since(pc_t0), id + 1);
            sem_down(&pc_empty);
        }
        mutex_lock(&pc_mutex);
        int item = ++pc_next_item;
        pc_buf[pc_in] = item;
        pc_in = (pc_in + 1) % PC_SLOTS;
        pc_count++;
        buffer_picture(pic);
        say(VGA_LIGHT_CYAN, "%5u ms  Ishlab-%u  qo'ydi   #%-2d  %s\n", ms_since(pc_t0), id + 1, item, pic);
        mutex_unlock(&pc_mutex);
        sem_up(&pc_full);                    /* "bitta mahsulot bor" */
    }
}

static void consumer(uint32_t id)
{
    char pic[PC_SLOTS + 3];
    for (int k = 0; k < PC_ITEMS; k++) {
        if (!sem_trydown(&pc_full)) {
            say(VGA_DARK_GREY, "%5u ms  Iste'mol-%u: bufer BO'SH - kutadi (sem_down to'la)\n",
                ms_since(pc_t0), id - 1);
            sem_down(&pc_full);
        }
        mutex_lock(&pc_mutex);
        int item = pc_buf[pc_out];
        pc_out = (pc_out + 1) % PC_SLOTS;
        pc_count--;
        buffer_picture(pic);
        say(VGA_YELLOW, "%5u ms  Iste'mol-%u oldi    #%-2d  %s\n", ms_since(pc_t0), id - 1, item, pic);
        mutex_unlock(&pc_mutex);
        sem_up(&pc_empty);                   /* "bitta joy bo'shadi" */
        timer_sleep_ms(rnd(60, 150));        /* mahsulotni ishlatish (sekinroq) */
    }
}

static void pc_thread(uint32_t id)
{
    if (id < 2)
        producer(id);
    else
        consumer(id);
}

static void demo_producer_consumer(void)
{
    say(VGA_WHITE, "ISHLAB CHIQARUVCHI - ISTE'MOLCHI: 2 + 2 oqim, %d o'rinli bufer\n", PC_SLOTS);
    say(VGA_LIGHT_GREY, "  semafor 'bo'sh' = %d (joylar), 'to'la' = 0 (mahsulotlar), mutex - bufer\n"
                        "  indekslari uchun. Ishlab chiqaruvchilar tezroq: bufer to'lib qoladi.\n\n", PC_SLOTS);
    pc_in = pc_out = pc_count = pc_next_item = 0;
    sem_init(&pc_empty, "bo'sh", PC_SLOTS);
    sem_init(&pc_full, "to'la", 0);
    mutex_init(&pc_mutex, "bufer", false);
    pc_t0 = timer_ticks();
    run_threads("pc", 4, pc_thread);
    say(VGA_LIGHT_GREEN, "\nJami: %d ishlab chiqarildi, %d iste'mol qilindi, buferda %d qoldi.\n",
        pc_next_item, 2 * PC_ITEMS, pc_count);
    say(VGA_LIGHT_GREY, "Kutishlar: ishlab chiqaruvchilar %u marta (bufer to'la), iste'molchilar %u marta\n"
                        "(bufer bo'sh). Hech kim band-kutishda (busy wait) protsessorni yemadi: ular uxladi.\n",
        pc_empty.waits, pc_full.waits);
}

/* -------------------------------- 3. Deadlock -------------------------------- */

static mutex_t dl_a, dl_b;

static void deadlock_thread(uint32_t id)
{
    mutex_t *first  = id == 0 ? &dl_a : &dl_b;   /* T1: printer -> skaner,  T2: skaner -> printer */
    mutex_t *second = id == 0 ? &dl_b : &dl_a;
    const char *me = task_current()->name;
    for (int attempt = 1; attempt <= 5; attempt++) {
        mutex_lock(first);
        say(VGA_LIGHT_CYAN, "%s: %s ni oldim, endi %s kerak...\n", me, first->name, second->name);
        timer_sleep_ms(100);                 /* ikkinchisi ham o'z birinchisini olib ulgursin */
        int r = mutex_lock(second);
        if (r == SYNC_OK) {
            say(VGA_LIGHT_GREEN, "%s: ikkalasi ham menda - ishlayapman\n", me);
            timer_sleep_ms(80);
            mutex_unlock(second);
            mutex_unlock(first);
            say(VGA_LIGHT_GREEN, "%s: tugatdim, ikkalasini bo'shatdim\n", me);
            return;
        }
        char cycle[240];
        describe_cycle(second, cycle, sizeof(cycle));
        say(VGA_LIGHT_RED, "%s: DEADLOCK aniqlandi! aylana: %s\n", me, cycle);
        say(VGA_YELLOW, "%s: %s ni qaytaraman (orqaga chekinish) va keyinroq qayta urinaman\n",
            me, first->name);
        mutex_unlock(first);                 /* aylanani uzamiz */
        timer_sleep_ms(250);
    }
}

static void demo_deadlock(void)
{
    say(VGA_WHITE, "DEADLOCK: ikki oqim ikki resursni teskari tartibda oladi\n");
    say(VGA_LIGHT_GREY, "  dl1: printer -> skaner,   dl2: skaner -> printer\n"
                        "  Coffman shartlari: o'zaro istisno, ushlab-kutish, tortib olinmaslik,\n"
                        "  aylanma kutish. Kernel mutex_lock da kutish grafidagi aylanani qidiradi.\n\n");
    mutex_init(&dl_a, "printer", false);
    mutex_init(&dl_b, "skaner", false);
    uint32_t t0 = timer_ticks();
    run_threads("dl", 2, deadlock_thread);
    say(VGA_LIGHT_GREY, "\nIkkala oqim ham tugadi (%u ms). Aniqlangan deadlocklar: %u.\n",
        ms_since(t0), dl_a.deadlocks + dl_b.deadlocks);
    say(VGA_LIGHT_GREY, "Aniqlagich bo'lmasa, ikkalasi abadiy kutib qolardi (tizimning shu qismi qotadi).\n");
}

/* --------------------------- 4. Ovqatlanayotgan faylasuflar --------------------------- */

#define PHILS 5
#define MEALS 2

static mutex_t      forks[PHILS];
static char         fork_names[PHILS][8];
static volatile char phil_state[PHILS];      /* 'o' — o'ylayapti, 'k' — kutyapti, 'Y' — yeyapti */
static bool         phil_ordered;
static uint32_t     phil_deadlocks;

static void phil_line(int id, const char *what, uint8_t color)
{
    char st[PHILS * 2 + 1];
    for (int i = 0; i < PHILS; i++) {
        st[i * 2] = phil_state[i];
        st[i * 2 + 1] = ' ';
    }
    st[PHILS * 2] = 0;
    say(color, "[ %s]  F%d %s\n", st, id + 1, what);
}

static void philosopher(uint32_t arg)
{
    int id = (int)arg;
    int left = id, right = (id + 1) % PHILS;
    int first = left, second = right;
    if (phil_ordered && first > second) {    /* tartib: har doim kichik raqamli vilkadan boshlash */
        first = right;
        second = left;
    }
    for (int meal = 0; meal < MEALS; ) {
        phil_state[id] = 'o';
        timer_sleep_ms(rnd(10, 40));
        phil_state[id] = 'k';
        mutex_lock(&forks[first]);
        timer_sleep_ms(60);                  /* hamma birinchi vilkasini olib ulgursin */
        if (mutex_lock(&forks[second]) != SYNC_OK) {
            phil_deadlocks++;
            char cycle[240];
            describe_cycle(&forks[second], cycle, sizeof(cycle));
            phil_line(id, "DEADLOCK! vilkani qaytaradi", VGA_LIGHT_RED);
            say(VGA_DARK_GREY, "  %s\n", cycle);
            mutex_unlock(&forks[first]);
            timer_sleep_ms(rnd(50, 150));
            continue;
        }
        phil_state[id] = 'Y';
        phil_line(id, "yeyapti", VGA_LIGHT_GREEN);
        timer_sleep_ms(rnd(40, 80));
        meal++;
        phil_state[id] = 'o';
        mutex_unlock(&forks[second]);
        mutex_unlock(&forks[first]);
    }
    phil_state[id] = '-';
    phil_line(id, "to'ydi", VGA_LIGHT_GREY);
}

static void demo_philosophers(bool ordered)
{
    say(VGA_WHITE, "OVQATLANAYOTGAN FAYLASUFLAR (Dijkstra, 1965): 5 faylasuf, 5 vilka\n");
    say(VGA_LIGHT_GREY, "  Yeyish uchun ikkala yonidagi vilka kerak. Holat: o - o'ylayapti, k - kutyapti,\n"
                        "  Y - yeyapti, - - tugatdi.\n");
    if (ordered)
        say(VGA_LIGHT_GREY, "  Yechim: vilkalar raqamlangan, har kim avval KICHIK raqamlisini oladi -\n"
                            "  aylana hosil bo'lmaydi (aylanma kutish sharti buziladi).\n\n");
    else
        say(VGA_LIGHT_GREY, "  Sodda usul: hamma avval chap vilkani oladi -> 5 tali aylana (deadlock).\n"
                            "  Kernel buni aniqlaydi, faylasuf vilkasini qaytarib qayta urinadi.\n"
                            "  (Yechimni ko'rish: sinx faylasuflar tartib)\n\n");
    phil_ordered = ordered;
    phil_deadlocks = 0;
    for (int i = 0; i < PHILS; i++) {
        ksnprintf(fork_names[i], sizeof(fork_names[i]), "vilka%d", i + 1);
        mutex_init(&forks[i], fork_names[i], false);
        phil_state[i] = 'o';
    }
    uint32_t t0 = timer_ticks();
    run_threads("F", PHILS, philosopher);
    say(VGA_LIGHT_GREEN, "\nHamma %d martadan ovqatlandi (%u ms). Aniqlangan deadlocklar: %u.\n",
        MEALS, ms_since(t0), phil_deadlocks);
}

/* --------------------------------- Buyruq --------------------------------- */

void cmd_sinx(const char *args)
{
    static bool ready = false;
    if (!ready) {
        mutex_init(&console, "konsol", false);
        ready = true;
    }
    char what[16], opt[16];
    const char *rest = shell_next_arg(args, what, sizeof(what));
    shell_next_arg(rest, opt, sizeof(opt));

    if (strcmp(what, "poyga") == 0)
        demo_race();
    else if (strcmp(what, "ishlab") == 0)
        demo_producer_consumer();
    else if (strcmp(what, "deadlock") == 0)
        demo_deadlock();
    else if (strcmp(what, "faylasuflar") == 0)
        demo_philosophers(strcmp(opt, "tartib") == 0);
    else {
        kprintf("Sinxronizatsiya namoyishlari (v0.8):\n"
                "  sinx poyga                race condition: qulfsiz va mutex bilan\n"
                "  sinx ishlab               producer-consumer (semafor + mutex)\n"
                "  sinx deadlock             deadlock va uni aniqlash\n"
                "  sinx faylasuflar [tartib] ovqatlanayotgan faylasuflar\n"
                "Holat: cat /proc/sync (mutexlar, semaforlar, kanallar)\n");
    }
}
