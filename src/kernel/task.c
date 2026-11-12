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
 *
 * STEK QO'RIQCHISI (v0.6.1)
 * Har bir vazifaning kernel steki 16 KB. Chuqur rekursiya yoki katta lokal
 * massiv uni to'ldirib yuborsa, stek qo'shni xotirani jimgina buzadi — eng
 * topish qiyin xatolardan biri. Shuning uchun stek tubiga maxsus qiymat
 * (canary — "kanareyka") yozib qo'yamiz va har kontekst almashtirishda
 * tekshiramiz: o'zgargan bo'lsa — stek to'lib ketgan, kernel panic.
 *
 * FPU: "LAZY" KONTEKST ALMASHTIRISH (v0.6.1)
 * Suzuvchi nuqtali hisoblar (double) protsessorning alohida FPU registrlarida
 * bajariladi (108 bayt holat). Ularni HAR almashtirishda saqlash isrof:
 * ko'p dasturlar FPU ni umuman ishlatmaydi. Yechim:
 *   - vazifa almashganda CR0 dagi TS bitini yoqamiz ("FPU boshqaniki");
 *   - dastur FPU buyrug'ini bajarmoqchi bo'lsa, protsessor #7 exception beradi;
 *   - shundagina eski egasining holatini saqlab (fnsave), yangisinikini
 *     tiklaymiz (frstor) va dastur davom etadi.
 * Natija: FPU ishlatmaydigan dasturlar uchun xarajat nol.
 */
#include "task.h"
#include "memory.h"
#include "string.h"
#include "drivers.h"
#include "config.h"
#include "irq.h"
#include "cpu.h"
#include "vfs.h"
#include "kernel.h"
#include "klog.h"

static task_t tasks[MAX_TASKS];
static task_t *current = 0;
static task_t *idle_task = 0;
static task_t *shell_task = 0;
static uint32_t next_pid = 0;
static uint32_t slice_left = SCHED_QUANTUM;
static uint32_t switch_count = 0;
static volatile bool need_resched = false;
static bool active = false;

static uint32_t foreground_pid = 0;   /* shell kutayotgan (oldingi plandagi) jarayon */

static bool    fpu_present = false;
static task_t *fpu_owner = 0;         /* FPU registrlarida hozir kimning holati turibdi */

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
    shell_task->stack_guard = (uint32_t *)boot_stack_bottom;   /* boot.s dagi 16 KB stek */
    *shell_task->stack_guard = STACK_CANARY;
    current = shell_task;

    uint32_t idle_pid = task_create("idle", idle_loop, 0);
    for (int i = 0; i < MAX_TASKS; i++)
        if (tasks[i].state != TASK_UNUSED && tasks[i].pid == idle_pid)
            idle_task = &tasks[i];
    if (!idle_pid || !idle_task)
        panic("sched_init: idle vazifasini yaratib bo'lmadi");
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
    t->stack_guard = (uint32_t *)stack;
    *t->stack_guard = STACK_CANARY;
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
static void reap(uint32_t only_pid)
{
    for (int i = 0; i < MAX_TASKS; i++) {
        uint32_t flags = irq_save();
        task_t *t = &tasks[i];
        if (only_pid && t->pid != only_pid) {
            irq_restore(flags);
            continue;
        }
        uint8_t *stack = 0;
        uint32_t *dir = 0;
        uint32_t pid = 0;
        if (t->state == TASK_ZOMBIE && t != current && t->pid != foreground_pid) {
            stack = t->stack;
            dir = t->page_dir;
            pid = t->pid;
            t->stack = 0;
            t->page_dir = 0;
            t->state = TASK_UNUSED;
        }
        irq_restore(flags);
        if (stack)
            kfree(stack);
        if (dir)
            paging_free_directory(dir);       /* jarayonning butun xotirasi qaytariladi */
        if (pid)
            vfs_close_owner(pid);             /* yopilmay qolgan fayllari */
    }
}

void task_reap(void)             { reap(0); }
void task_reap_pid(uint32_t pid) { if (pid) reap(pid); }

/* Vazifani kutayotganlarni (task_wait) uyg'otish */
static void wake_waiters(uint32_t pid)
{
    for (int i = 0; i < MAX_TASKS; i++)
        if (tasks[i].state == TASK_BLOCKED && tasks[i].wait_pid == pid) {
            tasks[i].wait_pid = 0;
            task_unblock(&tasks[i]);
        }
}

/* Vazifani ZOMBIE qilish. Uzilishlar o'chiq holda chaqiriladi */
static void make_zombie(task_t *t, int32_t code)
{
    t->state = TASK_ZOMBIE;
    t->exit_code = code;
    if (fpu_owner == t)
        fpu_owner = 0;                 /* uning FPU holati endi hech kimga kerak emas */
    keyboard_cancel_waiter(t);
    wake_waiters(t->pid);
    need_resched = true;
}

task_t *task_find(uint32_t pid)
{
    for (int i = 0; i < MAX_TASKS; i++)
        if (tasks[i].state != TASK_UNUSED && tasks[i].pid == pid)
            return &tasks[i];
    return 0;
}

bool task_kill(uint32_t pid)
{
    if (pid == 0 || pid == shell_task->pid)
        return false;                  /* idle va shell ni o'ldirib bo'lmaydi */

    uint32_t flags = irq_save();
    task_t *t = task_find(pid);
    if (t && t->state != TASK_ZOMBIE) {
        /* Vazifa hozir ishlamayapti (ishlayotgani — biz), shuning uchun
           uni darhol to'xtatsa bo'ladi: endi scheduler uni tanlamaydi */
        make_zombie(t, EXIT_KILLED);
        strcpy(t->note, "o'ldirildi (kill)");
        irq_restore(flags);
        task_reap();
        return true;
    }
    irq_restore(flags);
    return false;
}

/* Ctrl+C: klaviatura uzilishi ichidan chaqiriladi, shuning uchun xotirani
   bo'shatmaymiz — faqat ZOMBIE qilamiz, qolganini shell (task_reap) qiladi */
void task_signal_kill(uint32_t pid, int32_t code)
{
    uint32_t flags = irq_save();
    task_t *t = task_find(pid);
    if (t && t->user && t->state != TASK_ZOMBIE)
        make_zombie(t, code);
    irq_restore(flags);
}

void task_exit_code(int32_t code)
{
    __asm__ volatile ("cli");
    make_zombie(current, code);
    task_yield();                      /* boshqa vazifaga o'tamiz va bu yerga qaytmaymiz */
    for (;;)
        __asm__ volatile ("hlt");
}

void task_exit(void)
{
    task_exit_code(0);
}

/* pid tugashini kutish. Chiqish kodini qaytaradi (jarayon topilmasa — 0) */
int32_t task_wait(uint32_t pid)
{
    for (;;) {
        uint32_t flags = irq_save();
        task_t *t = task_find(pid);
        if (!t) {
            irq_restore(flags);
            return 0;
        }
        if (t->state == TASK_ZOMBIE) {
            int32_t code = t->exit_code;
            irq_restore(flags);
            return code;
        }
        current->wait_pid = pid;
        task_block();                  /* make_zombie -> wake_waiters bizni uyg'otadi */
        irq_restore(flags);
    }
}

void task_set_foreground(uint32_t pid) { foreground_pid = pid; }
uint32_t task_foreground(void)         { return foreground_pid; }

/*
 * User jarayon yaratish. Kernel stekida uzilish kadri quriladi, lekin bu
 * safar segmentlar USER (ring 3) va useresp/ss ham bor — iret protsessorni
 * ring 3 ga, dasturning entry nuqtasiga va uning o'z stekiga o'tkazadi.
 */
uint32_t task_create_user(const char *name, uint32_t *page_dir, uint32_t entry,
                          uint32_t user_esp, uint32_t brk, uint32_t pages)
{
    uint8_t *stack = kmalloc(TASK_STACK_SIZE);   /* jarayonning KERNEL steki (syscall va uzilishlar uchun) */
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
    t->pid        = next_pid++;
    strncpy(t->name, name, TASK_NAME_LEN - 1);
    t->stack      = stack;
    t->stack_guard = (uint32_t *)stack;
    *t->stack_guard = STACK_CANARY;
    t->user       = true;
    t->page_dir   = page_dir;
    t->brk_start  = brk;
    t->brk        = brk;
    t->user_pages = pages;
    t->parent_pid = current->pid;

    /* Kadr stek tepasiga TO'G'RI joylashadi: keyingi uzilishlarda protsessor
       aynan shu joyga (TSS.esp0) yozadi */
    registers_t *r = (registers_t *)(stack + TASK_STACK_SIZE - sizeof(registers_t));
    memset(r, 0, sizeof(*r));
    r->gs = r->fs = r->es = r->ds = USER_DS;
    r->cs      = USER_CS;
    r->eip     = entry;
    r->eflags  = 0x202;                       /* IF=1 */
    r->useresp = user_esp;
    r->ss      = USER_DS;
    t->regs    = r;
    t->state   = TASK_READY;

    uint32_t pid = t->pid;
    irq_restore(flags);
    return pid;
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
        if (tasks[i].state == TASK_SLEEPING && (int32_t)(now - tasks[i].wake_tick) >= 0) {
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

/* Joriy vazifaning steki butunmi? (qo'riqchi qiymat joyida va joy qolgan) */
static void check_stack(const task_t *t, const registers_t *r)
{
    if (!t->stack_guard)
        return;
    if (*t->stack_guard != STACK_CANARY)
        panic("kernel steki to'lib ketdi: PID %u (%s)", t->pid, t->name);
    if ((uint32_t)r < (uint32_t)t->stack_guard + STACK_RED_ZONE)
        panic("kernel steki deyarli to'ldi: PID %u (%s), ESP 0x%x, stek tubi 0x%x",
              t->pid, t->name, (uint32_t)r, (uint32_t)t->stack_guard);
}

static registers_t *switch_to(registers_t *r)
{
    need_resched = false;
    check_stack(current, r);
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

    /* v0.5: jarayonning xotirasi (CR3) va kernel steki (TSS.esp0) */
    if (current->user)
        tss_set_kernel_stack((uint32_t)current->stack + TASK_STACK_SIZE);
    paging_switch(current->page_dir ? current->page_dir : paging_kernel_directory());

    /* FPU: egasiga qaytgan bo'lsak — ochamiz, aks holda "qulflaymiz" (TS = 1) */
    if (fpu_present) {
        if (current == fpu_owner) {
            __asm__ volatile ("clts");
        } else {
            uint32_t cr0;
            __asm__ volatile ("mov %%cr0, %0" : "=r"(cr0));
            cr0 |= 0x08;
            __asm__ volatile ("mov %0, %%cr0" : : "r"(cr0));
        }
    }
    return current->regs;              /* isr_common shu stekka o'tadi */
}

/* Har bir IRQ oxirida idt.c chaqiradi */
registers_t *sched_maybe_switch(registers_t *r)
{
    if (!active)
        return r;
    check_stack(current, r);           /* har bir uzilishda: to'lib ketishni erta ushlash */
    if (!need_resched)
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

/* ------------------------------- FPU (v0.6.1) ------------------------------- */

#define CR0_MP 0x02      /* FPU bor: wait/fwait ham TS ni hisobga olsin */
#define CR0_EM 0x04      /* FPU yo'q: har bir FPU buyrug'i exception bersin */
#define CR0_TS 0x08      /* "vazifa almashdi": keyingi FPU buyrug'i #7 beradi */
#define CR0_NE 0x20      /* FPU xatolari exception #16 orqali (zamonaviy usul) */

void fpu_init(void)
{
    uint32_t a = 1, b, c = 0, d;
    __asm__ volatile ("cpuid" : "+a"(a), "=b"(b), "+c"(c), "=d"(d));
    fpu_present = d & 1;                   /* CPUID.1:EDX bit 0 — FPU */

    uint32_t cr0;
    __asm__ volatile ("mov %%cr0, %0" : "=r"(cr0));
    if (fpu_present) {
        cr0 &= ~(uint32_t)(CR0_EM | CR0_TS);
        cr0 |= CR0_MP | CR0_NE;
        __asm__ volatile ("mov %0, %%cr0" : : "r"(cr0));
        __asm__ volatile ("fninit");
        cr0 |= CR0_TS;                     /* birinchi FPU buyrug'igacha yopiq turadi */
    } else {
        cr0 |= CR0_EM;
    }
    __asm__ volatile ("mov %0, %%cr0" : : "r"(cr0));
}

/*
 * Exception #7 (Device Not Available): dastur FPU buyrug'ini bajardi, lekin
 * FPU registrlarida boshqa dasturning holati turibdi. Almashtirib beramiz.
 */
bool fpu_handle_nm(registers_t *r)
{
    if (!fpu_present || (r->cs & 3) != 3)
        return false;                      /* FPU yo'q yoki kernel ishlatdi (xato) */

    __asm__ volatile ("clts");
    if (fpu_owner != current) {
        if (fpu_owner)
            __asm__ volatile ("fnsave (%0)" : : "r"(fpu_owner->fpu_state) : "memory");
        if (current->fpu_used) {
            __asm__ volatile ("frstor (%0)" : : "r"(current->fpu_state) : "memory");
        } else {
            __asm__ volatile ("fninit");   /* toza holat: oldingi dasturning sonlari ko'rinmaydi */
            current->fpu_used = true;
        }
        fpu_owner = current;
    }
    return true;
}
