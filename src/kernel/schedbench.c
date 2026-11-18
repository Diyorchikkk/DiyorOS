/*
 * schedbench.c — rejalashtirish algoritmlarini taqqoslash tajribalari (v0.11).
 *
 * "reja taqqos" ikki qismdan iborat:
 *
 *   A) Tekshiruv: faqat 4 ta hisob vazifasi — qisqa 95, orta 195, uzun 395 ms
 *      (ustuvorlik 5), fon 795 ms (ustuvorlik 2), hammasi bir paytda keladi.
 *      Natija shu yerning o'zida NAZARIY model (ideal RR / PRIO, almashtirish
 *      bepul) bilan solishtiriladi. Farq kichik bo'lsa — scheduler algoritmni
 *      to'g'ri bajaradi va o'lchov usuli ishonchli.
 *      (Ish hajmlari kvantga karrali emas: aks holda vazifa oxirgi kvanti tugashi
 *      bilan "tugadim" deyishga ulgurmay navbatga qaytarilardi.)
 *
 *   B) Aralash yuklama: o'sha 4 ta vazifa + 2 ta interaktiv vazifa: 30 marta
 *      "20 ms uxlash (foydalanuvchini kutish) + 1 ms ish". Bu yerda asosiy
 *      ko'rsatkich — uyg'ongan interaktiv vazifa qancha tez protsessorga chiqadi.
 *
 * O'lchanadi (TSC, mikrosekund aniqlikda):
 *   aylanish (turnaround) = tugash - kelish
 *   kutish   (waiting)    = navbatda (READY) o'tgan jami vaqt
 *   javob    (response)   = kelishdan birinchi marta protsessorga chiqqungacha
 *   uyg'onish kechikishi  = interaktiv vazifa uyg'ongandan protsessorga chiqqungacha
 *   almashishlar          = kontekst almashtirishlar soni
 *
 * "reja ulush" — uchta cheksiz hisob vazifasi (ustuvorlik 2, 4, 8) 2 soniya:
 * har biri protsessorning qancha qismini oladi (algoritm "adolati").
 *
 * Vazifalar kernel oqimlari: dastur yuklash va syscall xarajatlari natijaga
 * aralashmaydi — faqat scheduler qarori o'lchanadi.
 */
#include "tests.h"
#include "task.h"
#include "tsc.h"
#include "drivers.h"
#include "irq.h"
#include "kprintf.h"
#include "string.h"
#include "config.h"

#define JOBS     6
#define CPU_JOBS 4

typedef struct {
    const char *name;
    bool        interactive;
    uint32_t    amount;          /* hisob: ms (protsessor vaqti); interaktiv: takrorlar */
    uint8_t     prio;            /* PRIO va LOTTERY uchun */
} job_spec_t;

static const job_spec_t spec[JOBS] = {
    { "qisqa",  false,  95, 5 },
    { "orta",   false, 195, 5 },
    { "uzun",   false, 395, 5 },
    { "fon",    false, 795, 2 },
    { "inter1", true,   30, 8 },
    { "inter2", true,   30, 8 },
};

static const struct {
    sched_policy_t policy;
    uint32_t       quantum;      /* tick (MLFQ — o'z kvantlari) */
    const char    *name;
} configs[] = {
    { SCHED_RR,      1,  "RR"       },
    { SCHED_RR,      5,  "RR"       },
    { SCHED_RR,      20, "RR"       },
    { SCHED_PRIO,    5,  "PRIO"     },
    { SCHED_MLFQ,    5,  "MLFQ"     },
    { SCHED_LOTTERY, 5,  "LOTEREYA" },
};
#define CONFIGS (sizeof(configs) / sizeof(configs[0]))

static volatile uint32_t sink;

/* Faqat hisoblash: o'zining aniq protsessor vaqti ms ga yetguncha */
static void cpu_work(uint32_t ms)
{
    uint64_t target = (uint64_t)tsc_khz() * ms;
    while (task_cpu_cycles() < target)
        for (int i = 0; i < 1000; i++)
            sink++;
}

/* ~us mikrosekund hisoblash (TSC bo'yicha) */
static void busy_us(uint32_t us)
{
    uint64_t end = rdtsc() + udiv64((uint64_t)tsc_khz() * us, 1000);
    while (rdtsc() < end)
        sink++;
}

static void job_entry(uint32_t i)
{
    if (!spec[i].interactive) {
        cpu_work(spec[i].amount);
        return;
    }
    for (uint32_t r = 0; r < spec[i].amount; r++) {
        task_sleep_ticks(2);                 /* 20 ms "foydalanuvchini kutish" */
        busy_us(1000);                       /* 1 ms javob tayyorlash */
    }
}

typedef struct {
    uint32_t turnaround[JOBS];   /* ms */
    uint32_t waiting[JOBS];      /* ms */
    uint32_t response[JOBS];     /* ms */
    uint32_t lat_avg_us, lat_max_us;
    uint32_t switches;
} result_t;

static uint32_t ms_of(uint64_t cycles)
{
    return tsc_to_us(cycles) / 1000;
}

static void run_once(int c, bool mixed, result_t *res)
{
    sched_set_policy(configs[c].policy);
    sched_set_quantum(configs[c].quantum);
    int jobs = mixed ? JOBS : CPU_JOBS;

    uint32_t pids[JOBS] = { 0 };
    uint32_t sw0 = sched_switch_count();
    uint32_t flags = irq_save();             /* hamma bir paytda "keladi" */
    for (int i = 0; i < jobs; i++) {
        pids[i] = task_create(spec[i].name, job_entry, (uint32_t)i);
        task_set_prio(pids[i], spec[i].prio);
    }
    irq_restore(flags);
    for (int i = 0; i < jobs; i++)
        if (pids[i])
            task_wait(pids[i]);

    memset(res, 0, sizeof(*res));
    uint64_t lat_sum = 0, lat_max = 0;
    uint32_t wakeups = 0;
    for (int i = 0; i < jobs; i++) {
        task_t *t = task_find(pids[i]);
        if (!t)
            continue;
        res->turnaround[i] = ms_of(t->end_tsc - t->start_tsc);
        res->waiting[i]    = ms_of(t->ready_wait);
        res->response[i]   = ms_of(t->first_wait);
        if (spec[i].interactive) {
            lat_sum += t->wake_lat_sum;
            wakeups += t->wakeups;
            if (t->wake_lat_max > lat_max)
                lat_max = t->wake_lat_max;
        }
        task_reap_pid(pids[i]);
    }
    res->lat_avg_us = wakeups ? tsc_to_us(udiv64(lat_sum, wakeups)) : 0;
    res->lat_max_us = tsc_to_us(lat_max);
    res->switches   = sched_switch_count() - sw0;
}

static uint32_t avg_cpu(const uint32_t *v)
{
    uint32_t s = 0;
    for (int i = 0; i < CPU_JOBS; i++)
        s += v[i];
    return s / CPU_JOBS;
}

/*
 * Nazariy model: ideal RR (yoki qat'iy ustuvorlik, tenglar orasida RR), FIFO
 * navbat, almashtirish bepul. O'rtacha aylanish vaqti (ms). MLFQ va lotereya
 * uchun oddiy yopiq model yo'q — 0.
 */
static uint32_t theory(int c)
{
    if (configs[c].policy != SCHED_RR && configs[c].policy != SCHED_PRIO)
        return 0;
    uint32_t q = configs[c].quantum * (1000 / TIMER_HZ);
    uint32_t left[CPU_JOBS], end[CPU_JOBS], t = 0;
    int queue[CPU_JOBS], n = CPU_JOBS;
    for (int i = 0; i < CPU_JOBS; i++) {
        left[i] = spec[i].amount;
        queue[i] = i;                                /* FIFO: kelish tartibida */
    }
    while (n) {
        int pick = 0;
        if (configs[c].policy == SCHED_PRIO)         /* eng yuqori ustuvorlik, tenglar — FIFO */
            for (int k = 1; k < n; k++)
                if (spec[queue[k]].prio > spec[queue[pick]].prio)
                    pick = k;
        int j = queue[pick];
        for (int k = pick; k < n - 1; k++)
            queue[k] = queue[k + 1];
        n--;
        uint32_t run = left[j] < q ? left[j] : q;
        t += run;
        left[j] -= run;
        if (left[j])
            queue[n++] = j;                          /* navbat oxiriga */
        else
            end[j] = t;
    }
    uint32_t sum = 0;
    for (int i = 0; i < CPU_JOBS; i++)
        sum += end[i];
    return sum / CPU_JOBS;
}

static void quantum_label(int c, char *q, uint32_t size)
{
    if (configs[c].policy == SCHED_MLFQ)
        strlcpy(q, "10-40", size);
    else
        ksnprintf(q, size, "%u", configs[c].quantum * (1000 / TIMER_HZ));
}

static void csv_line(char part, int c, int r, const result_t *one, int jobs)
{
    for (int i = 0; i < jobs; i++)
        kprintf("CSV,%c,%s,%u,%d,%s,%u,%u,%u,%u,%u,%u\n", part, configs[c].name,
                configs[c].policy == SCHED_MLFQ ? 0 : configs[c].quantum * (1000 / TIMER_HZ),
                r + 1, spec[i].name, one->turnaround[i], one->waiting[i], one->response[i],
                spec[i].interactive ? one->lat_avg_us : 0, spec[i].interactive ? one->lat_max_us : 0,
                one->switches);
}

void schedbench_compare(int repeats, bool csv)
{
    if (repeats < 1)
        repeats = 1;
    if (repeats > 10)
        repeats = 10;
    if (!tsc_khz()) {
        kprintf("reja taqqos: protsessorda TSC yo'q - aniq o'lchab bo'lmaydi\n");
        return;
    }
    sched_policy_t old_policy = sched_policy();
    uint32_t old_quantum = sched_quantum();

    kprintf("A) TEKSHIRUV: 4 ta hisob vazifasi - 95, 195, 395 ms (ustuvorlik 5) va 795 ms (2).\n"
            "   O'rtacha aylanish vaqti nazariy model bilan solishtiriladi. Takror: %d\n\n", repeats);
    kprintf("%-9s%6s%10s%10s%8s%9s%8s%7s\n", "Algoritm", "Kvant", "Aylanish", "Nazariya", "Farq",
            "Kutish", "Javob", "Alm.");
    for (unsigned c = 0; c < CONFIGS; c++) {
        result_t sum, one;
        memset(&sum, 0, sizeof(sum));
        for (int r = 0; r < repeats; r++) {
            run_once((int)c, false, &one);
            for (int i = 0; i < CPU_JOBS; i++) {
                sum.turnaround[i] += one.turnaround[i];
                sum.waiting[i] += one.waiting[i];
                sum.response[i] += one.response[i];
            }
            sum.switches += one.switches;
            if (csv)
                csv_line('A', (int)c, r, &one, CPU_JOBS);
        }
        uint32_t ta = avg_cpu(sum.turnaround) / repeats, th = theory((int)c);
        char q[8], diff[12] = "-", tstr[12] = "-";
        quantum_label((int)c, q, sizeof(q));
        if (th) {
            int d = (int)(ta * 1000 / th) - 1000;          /* promille */
            int a = d < 0 ? -d : d;
            ksnprintf(diff, sizeof(diff), "%c%d.%d%%", d < 0 ? '-' : '+', a / 10, a % 10);
            ksnprintf(tstr, sizeof(tstr), "%u", th);
        }
        kprintf("%-9s%6s%10u%10s%8s%9u%8u%7u\n", configs[c].name, q, ta, tstr, diff,
                avg_cpu(sum.waiting) / repeats, avg_cpu(sum.response) / repeats, sum.switches / repeats);
    }

    kprintf("\nB) ARALASH: o'sha 4 ta + 2 ta interaktiv (30 x: 20 ms uxlash + 1 ms ish, ustuvorlik 8)\n\n");
    kprintf("%-9s%6s%9s%8s%7s%7s%15s%9s%6s\n", "Algoritm", "Kvant", "Aylanish", "Kutish", "Javob",
            "Qisqa", "Uyg'on.o'r,mks", "maks", "Alm.");
    for (unsigned c = 0; c < CONFIGS; c++) {
        result_t sum, one;
        memset(&sum, 0, sizeof(sum));
        for (int r = 0; r < repeats; r++) {
            run_once((int)c, true, &one);
            for (int i = 0; i < JOBS; i++) {
                sum.turnaround[i] += one.turnaround[i];
                sum.waiting[i] += one.waiting[i];
                sum.response[i] += one.response[i];
            }
            sum.lat_avg_us += one.lat_avg_us;
            if (one.lat_max_us > sum.lat_max_us)
                sum.lat_max_us = one.lat_max_us;
            sum.switches += one.switches;
            if (csv)
                csv_line('B', (int)c, r, &one, JOBS);
        }
        char q[8];
        quantum_label((int)c, q, sizeof(q));
        kprintf("%-9s%6s%9u%8u%7u%7u%15u%9u%6u\n", configs[c].name, q,
                avg_cpu(sum.turnaround) / repeats, avg_cpu(sum.waiting) / repeats,
                avg_cpu(sum.response) / repeats, sum.turnaround[0] / repeats,
                sum.lat_avg_us / repeats, sum.lat_max_us, sum.switches / repeats);
    }

    sched_set_policy(old_policy);
    sched_set_quantum(old_quantum);
    kprintf("\nVaqtlar ms da (hisob vazifalari bo'yicha o'rtacha). Kichigi yaxshi.\n"
            "Uyg'onish - interaktiv dastur (klaviatura, sichqoncha) qanchalik tez javob beradi.\n"
            "Qisqa - eng qisqa vazifa qachon tugadi.\n");
}

/* ----------------------------- Ulush tajribasi ----------------------------- */

static volatile bool share_stop;

static void share_entry(uint32_t arg)
{
    (void)arg;
    while (!share_stop)
        for (int i = 0; i < 1000; i++)
            sink++;
}

void schedbench_share(bool csv)
{
    static const uint8_t prios[3] = { 2, 4, 8 };
    static const sched_policy_t pols[] = { SCHED_RR, SCHED_PRIO, SCHED_MLFQ, SCHED_LOTTERY };
    static const char *names[] = { "RR", "PRIO", "MLFQ", "LOTEREYA" };
    sched_policy_t old_policy = sched_policy();
    uint32_t old_quantum = sched_quantum();

    kprintf("Uchta cheksiz hisob vazifasi 2 soniya: A (ustuvorlik 2), B (4), C (8).\n"
            "Protsessor ulushi (%%):\n\n%-10s%8s%8s%8s\n", "Algoritm", "A", "B", "C");
    for (int p = 0; p < 4; p++) {
        sched_set_policy(pols[p]);
        sched_set_quantum(1);                /* 10 ms: lotereyada 200 ta tortish — tasodif kamroq */
        share_stop = false;
        uint32_t pids[3];
        uint32_t flags = irq_save();
        for (int i = 0; i < 3; i++) {
            pids[i] = task_create("ulush", share_entry, (uint32_t)i);
            task_set_prio(pids[i], prios[i]);
        }
        irq_restore(flags);
        task_sleep_ticks(2 * TIMER_HZ);      /* shell (ustuvorlik 10) uyg'onib, to'xtatadi */
        share_stop = true;
        uint32_t ms[3], total = 0;
        for (int i = 0; i < 3; i++) {
            task_wait(pids[i]);
            task_t *t = task_find(pids[i]);
            ms[i] = t ? ms_of(t->run_cycles) : 0;
            total += ms[i];
            task_reap_pid(pids[i]);
        }
        if (!total)
            total = 1;
        kprintf("%-10s%7u%%%7u%%%7u%%\n", names[p], ms[0] * 100 / total,
                ms[1] * 100 / total, ms[2] * 100 / total);
        if (csv)
            kprintf("CSV,U,%s,%u,%u,%u\n", names[p], ms[0], ms[1], ms[2]);
    }
    sched_set_policy(old_policy);
    sched_set_quantum(old_quantum);
    kprintf("%-10s%7u%%%7u%%%7u%%   (lotereya nazariyasi: chiptalar 2:4:8)\n", "kutilgan", 14, 29, 57);
}
