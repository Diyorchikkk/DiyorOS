/*
 * sync.c — sinxronizatsiya primitivlari (v0.8).
 *
 * MUAMMO
 * Bir nechta vazifa bitta ma'lumotni o'zgartirsa, natija ular qaysi tartibda
 * ishlaganiga bog'liq bo'lib qoladi (race condition). Masalan "counter++":
 *     1) counter ni o'qish   2) 1 qo'shish   3) yozib qo'yish
 * 1- va 3-qadam orasida taymer vazifani almashtirsa, ikkinchi vazifaning
 * o'zgarishlari yo'qoladi ("sinx poyga" buyrug'i buni ko'rsatadi).
 *
 * v0.6.1 gacha yechim: uzilishlarni o'chirish (irq_save). Bitta protsessorda
 * ishlaydi, lekin qimmat: uzilishlar o'chiq paytda taymer ham, klaviatura ham,
 * sichqoncha ham kutadi. Disk bilan ishlash millisekundlab davom etadi —
 * shu paytda butun tizim "qotib" qoladi.
 *
 * MUTEX (mutual exclusion — o'zaro istisno)
 * Qulf: bir paytda faqat bitta vazifa ushlay oladi. Band bo'lsa, so'ragan
 * vazifa UXLAYDI (BLOCKED) va protsessor boshqalarga beriladi; uzilishlar
 * yoqiq qoladi. Qulf bo'shaganda navbatdagi birinchi vazifa uyg'otiladi va
 * qulf to'g'ridan-to'g'ri unga topshiriladi (handoff) — navbat adolatli (FIFO),
 * hech kim "och qolmaydi" (starvation yo'q).
 *
 * SEMAFOR (Dijkstra, 1965)
 * Hisoblagichli qulf: "N ta ruxsatnoma". down (P) — ruxsatnoma olish, yo'q
 * bo'lsa kutish; up (V) — qaytarish va kutayotganni uyg'otish. N = 1 bo'lsa
 * mutexga o'xshaydi; N = bufer hajmi bo'lsa — producer-consumer masalasi.
 *
 * DEADLOCK ANIQLASH
 * Deadlock — vazifalar bir-birini aylana bo'ylab kutib qolishi:
 *     T1 A ni ushlab, B ni kutyapti;  T2 B ni ushlab, A ni kutyapti.
 * mutex_lock kutishga o'tishdan oldin "kim kimni kutyapti" zanjiri bo'ylab
 * yuradi (resurslarni taqsimlash grafi): mutex -> egasi -> u kutayotgan mutex
 * -> uning egasi ... Zanjir o'zimizga qaytsa — aylana, ya'ni deadlock. Bunday
 * holda kutmaymiz, SYNC_EDEADLK qaytaramiz (Linux da: lockdep, pthread ning
 * ERRORCHECK mutexi).
 *
 * MUHIM QOIDA: kutish ro'yxatini tekshirish va vazifani uxlatish bo'linmas
 * bo'lishi kerak. Aks holda "tekshirdim — bo'sh emas — (shu payt egasi
 * qulfni bo'shatdi va hech kimni uyg'otmadi) — uxladim" va vazifa abadiy
 * uxlab qoladi (lost wakeup). Shuning uchun bu fayldagi ichki amallar juda
 * qisqa muddatga uzilishlarni o'chiradi — bu yerda irq_save qulfni AMALGA
 * OSHIRISH uchun ishlatiladi, ma'lumotni uzoq himoya qilish uchun emas.
 */
#include "sync.h"
#include "irq.h"
#include "kprintf.h"
#include "klog.h"
#include "string.h"

static mutex_t *all_mutexes = 0;
static semaphore_t *all_semaphores = 0;

/* ------------------------------ Kutish navbati ------------------------------ */

void wq_init(wait_queue_t *q)
{
    q->head = q->tail = 0;
}

static void wq_push(wait_queue_t *q, task_t *t)
{
    t->wq_next = 0;
    if (q->tail)
        q->tail->wq_next = t;
    else
        q->head = t;
    q->tail = t;
    t->wait_queue = q;
}

static task_t *wq_pop(wait_queue_t *q)
{
    task_t *t = q->head;
    if (!t)
        return 0;
    q->head = t->wq_next;
    if (!q->head)
        q->tail = 0;
    t->wq_next = 0;
    t->wait_queue = 0;
    return t;
}

static void wq_remove(wait_queue_t *q, task_t *t)
{
    task_t *prev = 0;
    for (task_t *cur = q->head; cur; prev = cur, cur = cur->wq_next) {
        if (cur != t)
            continue;
        if (prev)
            prev->wq_next = cur->wq_next;
        else
            q->head = cur->wq_next;
        if (q->tail == cur)
            q->tail = prev;
        t->wq_next = 0;
        t->wait_queue = 0;
        return;
    }
}

int wq_length(const wait_queue_t *q)
{
    int n = 0;
    for (task_t *t = q->head; t; t = t->wq_next)
        n++;
    return n;
}

/*
 * Joriy vazifani navbatga qo'yib, uxlatish. Uzilishlar o'chiq holda chaqiriladi.
 * Uyg'ongach (wq_wake_*, task_wake yoki signal) qaytadi. Chaqiruvchi shartni
 * QAYTA tekshirishi kerak (while tsiklida) — uyg'onish "soxta" bo'lishi mumkin.
 */
int wq_wait(wait_queue_t *q, const char *reason, bool interruptible)
{
    task_t *t = task_current();
    if (interruptible && task_should_abort())
        return SYNC_EINTR;
    wq_push(q, t);
    t->wait_reason = reason;
    t->state = TASK_BLOCKED;
    task_yield();                            /* boshqa vazifaga o'tamiz; uyg'onganda shu yerdan */
    if (t->wait_queue)                       /* signal bilan uyg'otilgan: navbatdan o'zimiz chiqamiz */
        wq_remove(t->wait_queue, t);
    t->wait_reason = 0;
    return (interruptible && task_should_abort()) ? SYNC_EINTR : SYNC_OK;
}

void wq_wake_one(wait_queue_t *q)
{
    uint32_t f = irq_save();
    task_t *t = wq_pop(q);
    if (t)
        task_wake(t);
    irq_restore(f);
}

void wq_wake_all(wait_queue_t *q)
{
    uint32_t f = irq_save();
    task_t *t;
    while ((t = wq_pop(q)) != 0)
        task_wake(t);
    irq_restore(f);
}

/* ----------------------------------- Mutex ----------------------------------- */

void mutex_init(mutex_t *m, const char *name, bool recursive)
{
    uint32_t f = irq_save();
    bool was_registered = m->registered;
    mutex_t *next = m->next;
    memset(m, 0, sizeof(*m));
    m->name = name;
    m->recursive = recursive;
    wq_init(&m->waiters);
    if (was_registered) {                    /* qayta ishga tushirish: ro'yxatda qoladi */
        m->registered = true;
        m->next = next;
    } else {
        m->registered = true;
        m->next = all_mutexes;
        all_mutexes = m;
    }
    irq_restore(f);
}

/*
 * Kutish zanjiri bo'ylab yurib, aylanani qidirish:
 *   m ning egasi -> u kutayotgan mutex -> uning egasi -> ... -> t ?
 */
static bool would_deadlock(const mutex_t *m, const task_t *t)
{
    const task_t *owner = m->owner;
    for (int steps = 0; owner && steps < MAX_TASKS; steps++) {
        if (owner == t)
            return true;
        const mutex_t *next = owner->waiting_mutex;
        if (!next)
            return false;
        owner = next->owner;
    }
    return false;
}

/* Deadlock zanjirini jurnalga yozish: "T2 -> A (T1) -> B (T2)" */
static void log_cycle(const mutex_t *m, const task_t *t)
{
    char buf[160];
    int n = ksnprintf(buf, sizeof(buf), "deadlock: %s(%u) -> %s", t->name, t->pid, m->name);
    const task_t *owner = m->owner;
    for (int steps = 0; owner && steps < MAX_TASKS && n < (int)sizeof(buf) - 32; steps++) {
        n += ksnprintf(buf + n, sizeof(buf) - n, " [%s(%u)]", owner->name, owner->pid);
        if (owner == t || !owner->waiting_mutex)
            break;
        n += ksnprintf(buf + n, sizeof(buf) - n, " -> %s", owner->waiting_mutex->name);
        owner = owner->waiting_mutex->owner;
    }
    klog("%s", buf);
}

int mutex_lock(mutex_t *m)
{
    if (!sched_active())
        return SYNC_OK;                      /* yuklanish paytida faqat bitta oqim bor */
    uint32_t f = irq_save();
    task_t *t = task_current();

    if (m->owner == t) {
        if (m->recursive) {
            m->depth++;
            irq_restore(f);
            return SYNC_OK;
        }
        panic("mutex '%s': %s(%u) o'zi ushlab turgan qulfni yana so'radi (o'z-o'zini deadlock)",
              m->name, t->name, t->pid);
    }

    if (!m->owner) {                         /* bo'sh — darhol olamiz */
        m->owner = t;
        m->depth = 1;
        m->acquisitions++;
        irq_restore(f);
        return SYNC_OK;
    }

    m->contentions++;
    if (would_deadlock(m, t)) {
        m->deadlocks++;
        log_cycle(m, t);
        irq_restore(f);
        return SYNC_EDEADLK;
    }

    t->waiting_mutex = m;
    while (m->owner != t)                    /* mutex_unlock qulfni bizga topshiradi (handoff) */
        wq_wait(&m->waiters, "mutex", false);
    t->waiting_mutex = 0;
    m->depth = 1;
    m->acquisitions++;
    irq_restore(f);
    return SYNC_OK;
}

bool mutex_trylock(mutex_t *m)
{
    if (!sched_active())
        return true;
    uint32_t f = irq_save();
    task_t *t = task_current();
    bool ok = false;
    if (!m->owner) {
        m->owner = t;
        m->depth = 1;
        m->acquisitions++;
        ok = true;
    } else if (m->owner == t && m->recursive) {
        m->depth++;
        ok = true;
    }
    irq_restore(f);
    return ok;
}

/* Qulfni navbatdagi kutayotganga topshirish (yoki ochish). Uzilishlar o'chiq */
static void handoff(mutex_t *m)
{
    task_t *next = wq_pop(&m->waiters);
    m->owner = next;
    m->depth = next ? 1 : 0;
    if (next)
        task_wake(next);
}

void mutex_unlock(mutex_t *m)
{
    if (!sched_active())
        return;
    uint32_t f = irq_save();
    task_t *t = task_current();
    if (m->owner != t)
        panic("mutex '%s': %s(%u) o'ziniki bo'lmagan qulfni ochmoqchi (egasi: %s)",
              m->name, t->name, t->pid, m->owner ? m->owner->name : "yo'q");
    if (--m->depth == 0)
        handoff(m);
    irq_restore(f);
}

bool mutex_owned(const mutex_t *m)
{
    return sched_active() && m->owner == task_current();
}

/* ---------------------------------- Semafor ---------------------------------- */

void sem_init(semaphore_t *s, const char *name, int32_t initial)
{
    uint32_t f = irq_save();
    bool was_registered = s->registered;
    semaphore_t *next = s->next;
    memset(s, 0, sizeof(*s));
    s->name = name;
    s->count = initial;
    wq_init(&s->waiters);
    s->registered = true;
    if (was_registered) {
        s->next = next;
    } else {
        s->next = all_semaphores;
        all_semaphores = s;
    }
    irq_restore(f);
}

int sem_down(semaphore_t *s)
{
    uint32_t f = irq_save();
    if (s->count <= 0)
        s->waits++;
    while (s->count <= 0) {
        if (wq_wait(&s->waiters, "semafor", true) == SYNC_EINTR) {
            irq_restore(f);
            return SYNC_EINTR;
        }
    }
    s->count--;
    irq_restore(f);
    return SYNC_OK;
}

bool sem_trydown(semaphore_t *s)
{
    uint32_t f = irq_save();
    bool ok = s->count > 0;
    if (ok)
        s->count--;
    irq_restore(f);
    return ok;
}

void sem_up(semaphore_t *s)
{
    uint32_t f = irq_save();
    s->count++;
    task_t *t = wq_pop(&s->waiters);
    if (t)
        task_wake(t);
    irq_restore(f);
}

/* ------------------------------ Vazifa o'limi ------------------------------ */

/*
 * "Robust" mutex: vazifa qulfni ushlab turgan holda o'lsa (masalan, kill bilan
 * to'xtatilgan kernel oqimi), qulf abadiy yopiq qolmasligi uchun uni keyingi
 * kutayotganga topshiramiz. Uzilishlar o'chiq holda chaqiriladi (make_zombie).
 */
void sync_task_died(task_t *t)
{
    if (t->wait_queue)
        wq_remove(t->wait_queue, t);
    t->waiting_mutex = 0;
    t->wait_reason = 0;
    for (mutex_t *m = all_mutexes; m; m = m->next)
        if (m->owner == t) {
            m->owner_died++;
            handoff(m);
        }
}

/* ---------------------------------- Hisobot ---------------------------------- */

int sync_report(char *b, uint32_t size)
{
    uint32_t f = irq_save();
    int n = ksnprintf(b, size, "MUTEXLAR\n%-16s %-14s %7s %8s %8s %8s\n",
                      "NOMI", "EGASI", "KUTUVCHI", "OLINGAN", "KUTILGAN", "DEADLOCK");
    for (mutex_t *m = all_mutexes; m && n < (int)size - 100; m = m->next) {
        char owner[24];
        if (m->owner)
            ksnprintf(owner, sizeof(owner), "%s(%u)", m->owner->name, m->owner->pid);
        else
            strcpy(owner, "-");
        n += ksnprintf(b + n, size - n, "%-16s %-14s %7d %8u %8u %8u\n", m->name, owner,
                       wq_length(&m->waiters), m->acquisitions, m->contentions, m->deadlocks);
    }
    n += ksnprintf(b + n, size - n, "\nSEMAFORLAR\n%-16s %8s %9s %8s\n", "NOMI", "QIYMAT", "KUTUVCHI", "KUTILGAN");
    for (semaphore_t *s = all_semaphores; s && n < (int)size - 80; s = s->next)
        n += ksnprintf(b + n, size - n, "%-16s %8d %9d %8u\n", s->name, s->count,
                       wq_length(&s->waiters), s->waits);
    irq_restore(f);
    return n;
}
