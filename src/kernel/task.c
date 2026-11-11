/*
 * task.c — scheduler (rejalashtiruvchi) va kontekst almashtirish.
 *
 * KONTEKST ALMASHTIRISH QANDAY ISHLAYDI
 * Har qanday uzilish (taymer, klaviatura...) kelganda interrupts.s dagi
 * isr_common barcha registrlarni joriy vazifaning STEKIGA saqlaydi va
 * C ga shu joy manzilini (registers_t *) beradi. Uzilishdan chiqishda
 * registrlar shu manzildan qayta tiklanadi.
 *
 * Fokus shunda: agar C funksiya BOSHQA vazifaning saqlangan registrlari
 * manzilini qaytarsa, isr_common stek ko'rsatkichini (ESP) o'sha joyga
 * o'zgartiradi va registrlarni o'sha vazifadan tiklaydi. "iret" esa
 * protsessorni o'sha vazifa to'xtagan joyga qaytaradi.
 * Ya'ni kontekst almashtirish = stek ko'rsatkichini almashtirish.
 *
 * ROUND-ROBIN
 * Har bir vazifaga 5 tick (50 ms) vaqt kvanti beriladi. Kvant tugasa,
 * taymer uzilishida keyingi READY vazifaga o'tamiz (navbat bilan, aylana).
 * Bu "preemptive" (majburiy) rejalashtirish: vazifa o'zi navbat bermasa ham,
 * taymer uni to'xtatadi. Cheksiz tsikl ichidagi vazifa ham tizimni osib qo'ymaydi.
 *
 * IDLE VAZIFA
 * Hech kim ishlashga tayyor bo'lmasa (hammasi uxlayapti yoki kutyapti),
 * idle ishlaydi: "hlt" bilan protsessorni keyingi uzilishgacha to'xtatadi.
 */
#include "task.h"
#include "memory.h"
#include "string.h"
#include "drivers.h"
#include "config.h"

static task_t tasks[MAX_TASKS];
static task_t *current = 0;
static task_t *idle_task = 0;
static task_t *shell_task = 0;
static uint32_t next_pid = 0;
static uint32_t slice_left = SCHED_QUANTUM;
static uint32_t switch_count = 0;
static volatile bool need_resched = false;
static bool active = false;

/* Uzilishlarni vaqtincha o'chirish va avvalgi holatni qaytarish */
static inline uint32_t irq_save(void)
{
    uint32_t flags;
    __asm__ volatile ("pushf; pop %0; cli" : "=r"(flags) : : "memory");
    return flags;
}

static inline void irq_restore(uint32_t flags)
{
    if (flags & 0x200)                 /* IF biti: avval yoqilgan bo'lsa */
        __asm__ volatile ("sti" : : : "memory");
}

/* Yangi vazifa birinchi marta shu yerdan boshlanadi */
static void task_trampoline(void)
{
    __asm__ volatile ("sti");
    current->entry(current->arg);
    task_exit();                       /* funksiya qaytsa — vazifa tugadi */
}

static void idle_loop(uint32_t arg)
{
    (void)arg;
    for (;;)
        __asm__ volatile ("sti; hlt");
}

static task_t *alloc_slot(void)
{
    for (int i = 0; i < MAX_TASKS; i++)
        if (tasks[i].state == TASK_UNUSED)
            return &tasks[i];
    return 0;
}

void sched_init(void)
{
    memset(tasks, 0, sizeof(tasks));

    /* Hozir ishlayotgan oqim (kernel_main -> shell) — 1-vazifa.
       Uning steki boot.s da ajratilgan, registrlari birinchi almashtirishda saqlanadi. */
    next_pid = 1;
    shell_task = &tasks[0];
    shell_task->pid = next_pid++;
    strcpy(shell_task->name, "shell");
    shell_task->state = TASK_RUNNING;
    current = shell_task;

    uint32_t idle_pid = task_create("idle", idle_loop, 0);
    for (int i = 0; i < MAX_TASKS; i++)
        if (tasks[i].pid == idle_pid)
            idle_task = &tasks[i];
    idle_task->pid = 0;                /* an'anaga ko'ra idle — PID 0 */
    next_pid = 2;

    active = true;
}

bool sched_active(void)
{
    return active;
}

uint32_t task_create(const char *name, task_entry_t entry, uint32_t arg)
{
    uint8_t *stack = kmalloc(TASK_STACK_SIZE);
    if (!stack)
        return 0;

    uint32_t flags = irq_save();
    task_t *t = alloc_slot();
    if (!t) {
        irq_restore(flags);
        kfree(stack);
        return 0;
    }

    memset(t, 0, sizeof(*t));
    t->pid   = next_pid++;
    strncpy(t->name, name, TASK_NAME_LEN - 1);
    t->stack = stack;
    t->entry = entry;
    t->arg   = arg;

    /*
     * Stek tepasida "soxta uzilish kadri" quramiz — go'yo bu vazifa avval
     * ishlagan-u, uzilish paytida to'xtatilgan. Scheduler unga o'tganda
     * isr_common registrlarni shu kadrdan tiklaydi va iret task_trampoline ga sakraydi.
     */
    registers_t *r = (registers_t *)(stack + TASK_STACK_SIZE - sizeof(registers_t) - 16);
    memset(r, 0, sizeof(*r));
    r->gs = r->fs = r->es = r->ds = 0x10;   /* kernel data segmenti */
    r->cs     = 0x08;                        /* kernel kod segmenti */
    r->eip    = (uint32_t)task_trampoline;
    r->eflags = 0x202;                       /* IF=1: uzilishlar yoqilgan */
    t->regs   = r;
    t->state  = TASK_READY;

    uint32_t pid = t->pid;
    irq_restore(flags);
    return pid;
}

/* ZOMBIE vazifalarning steklarini bo'shatish. Faqat shell kontekstidan chaqiriladi */
void task_reap(void)
{
    for (int i = 0; i < MAX_TASKS; i++) {
        uint32_t flags = irq_save();
        task_t *t = &tasks[i];
        uint8_t *stack = 0;
        if (t->state == TASK_ZOMBIE && t != current) {
            stack = t->stack;
            t->stack = 0;
            t->state = TASK_UNUSED;
        }
        irq_restore(flags);
        if (stack)
            kfree(stack);
    }
}

bool task_kill(uint32_t pid)
{
    if (pid == 0 || pid == shell_task->pid)
        return false;                  /* idle va shell ni o'ldirib bo'lmaydi */

    uint32_t flags = irq_save();
    for (int i = 0; i < MAX_TASKS; i++) {
        task_t *t = &tasks[i];
        if (t->state != TASK_UNUSED && t->state != TASK_ZOMBIE && t->pid == pid) {
            /* Vazifa hozir ishlamayapti (ishlayotgani — biz), shuning uchun
               uni darhol to'xtatsa bo'ladi: endi scheduler uni tanlamaydi */
            t->state = TASK_ZOMBIE;
            strcpy(t->note, "o'ldirildi (kill)");
            irq_restore(flags);
            task_reap();
            return true;
        }
    }
    irq_restore(flags);
    return false;
}

void task_exit(void)
{
    __asm__ volatile ("cli");
    current->state = TASK_ZOMBIE;
    task_yield();                      /* boshqa vazifaga o'tamiz va bu yerga qaytmaymiz */
    for (;;)
        __asm__ volatile ("hlt");
}

/* int 0x81 — "navbatni ber" uzilishi. U idt.c orqali sched_yield_handler ni chaqiradi */
void task_yield(void)
{
    __asm__ volatile ("int $0x81");
}

void task_sleep_ticks(uint32_t ticks)
{
    if (!active || ticks == 0)
        return;
    uint32_t flags = irq_save();
    current->wake_tick = timer_ticks() + ticks;
    current->state = TASK_SLEEPING;
    task_yield();
    irq_restore(flags);
}

void task_block(void)
{
    uint32_t flags = irq_save();
    current->state = TASK_BLOCKED;
    task_yield();
    irq_restore(flags);
}

void task_unblock(task_t *t)
{
    if (t && t->state == TASK_BLOCKED) {
        t->state = TASK_READY;
        need_resched = true;           /* tez javob berish uchun darhol almashamiz */
    }
}

task_t *task_current(void)
{
    return current;
}

task_t *task_get(int index)
{
    return (index >= 0 && index < MAX_TASKS) ? &tasks[index] : 0;
}

uint32_t sched_switch_count(void)
{
    return switch_count;
}

/* ---------------- Scheduler yuragi (uzilish ichida ishlaydi) ---------------- */

/* Har taymer tickida timer.c chaqiradi */
void sched_tick(void)
{
    if (!active)
        return;

    current->cpu_ticks++;
    uint32_t now = timer_ticks();

    /* Uxlayotganlarni uyg'otish */
    for (int i = 0; i < MAX_TASKS; i++)
        if (tasks[i].state == TASK_SLEEPING && now >= tasks[i].wake_tick) {
            tasks[i].state = TASK_READY;
            need_resched = true;
        }

    /* Har soniyada CPU% ni hisoblash: oxirgi 100 tickdan nechtasini olgan */
    if (now % TIMER_HZ == 0)
        for (int i = 0; i < MAX_TASKS; i++) {
            task_t *t = &tasks[i];
            t->cpu_percent = (t->cpu_ticks - t->cpu_prev) * 100 / TIMER_HZ;
            t->cpu_prev = t->cpu_ticks;
        }

    /* Kvant tugadimi? */
    if (slice_left > 0)
        slice_left--;
    if (slice_left == 0)
        need_resched = true;
}

/* Keyingi ishlaydigan vazifani tanlash: Round-Robin */
static task_t *pick_next(void)
{
    int start = (int)(current - tasks);
    for (int k = 1; k <= MAX_TASKS; k++) {
        task_t *t = &tasks[(start + k) % MAX_TASKS];
        if (t != idle_task && (t->state == TASK_READY ||
                               (t == current && t->state == TASK_RUNNING)))
            return t;
    }
    return idle_task;                  /* hech kim tayyor emas */
}

static registers_t *switch_to(registers_t *r)
{
    need_resched = false;
    current->regs = r;                 /* joriy vazifa qayerda to'xtaganini eslab qolamiz */

    task_t *next = pick_next();
    if (current->state == TASK_RUNNING)
        current->state = TASK_READY;

    if (next != current) {
        switch_count++;
        next->switches++;
    }
    current = next;
    current->state = TASK_RUNNING;
    slice_left = SCHED_QUANTUM;
    return current->regs;              /* isr_common shu stekka o'tadi */
}

/* Har bir IRQ oxirida idt.c chaqiradi */
registers_t *sched_maybe_switch(registers_t *r)
{
    if (!active || !need_resched)
        return r;
    return switch_to(r);
}

/* int 0x81 (task_yield) */
registers_t *sched_yield_handler(registers_t *r)
{
    if (!active)
        return r;
    return switch_to(r);
}
