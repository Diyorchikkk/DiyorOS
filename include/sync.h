/*
 * sync.h — sinxronizatsiya: kutish navbati, mutex, semafor (v0.8).
 */
#ifndef SYNC_H
#define SYNC_H

#include <stdint.h>
#include <stdbool.h>
#include "task.h"

#define SYNC_OK       0
#define SYNC_EDEADLK  (-35)   /* deadlock aniqlandi: kutish aylana hosil qilardi */
#define SYNC_EINTR    (-19)   /* kutish to'xtatildi (jarayonga signal keldi) = VFS_EINTR */

/* Kutish navbati: biror hodisani kutayotgan vazifalar ro'yxati (FIFO) */
typedef struct wait_queue {
    task_t *head, *tail;
} wait_queue_t;

typedef struct mutex {
    const char     *name;
    task_t         *owner;          /* qulfni ushlab turgan vazifa (0 — ochiq) */
    uint32_t        depth;          /* rekursiv qulf: egasi necha marta olgan */
    bool            recursive;
    wait_queue_t    waiters;
    uint32_t        acquisitions;   /* statistika: necha marta olingan */
    uint32_t        contentions;    /* ...shundan necha marta kutishga to'g'ri kelgan */
    uint32_t        deadlocks;      /* necha marta deadlock aniqlangan */
    uint32_t        owner_died;     /* egasi qulfni bo'shatmay o'lgan holatlar */
    bool            registered;
    struct mutex   *next;           /* barcha mutexlar ro'yxati (/proc/sync, robust) */
} mutex_t;

typedef struct semaphore {
    const char       *name;
    int32_t           count;        /* nechta "ruxsatnoma" bor */
    wait_queue_t      waiters;
    uint32_t          waits;        /* statistika: necha marta kutishga to'g'ri kelgan */
    bool              registered;
    struct semaphore *next;
} semaphore_t;

/* Kutish navbati — boshqa modullar (pipe, klaviatura) uchun ham.
   wq_wait ni uzilishlar O'CHIQ holda chaqirish kerak (irq_save ichida) */
void wq_init(wait_queue_t *q);
int  wq_wait(wait_queue_t *q, const char *reason, bool interruptible);   /* 0 yoki SYNC_EINTR */
void wq_wake_one(wait_queue_t *q);
void wq_wake_all(wait_queue_t *q);
int  wq_length(const wait_queue_t *q);

/* Mutex */
void mutex_init(mutex_t *m, const char *name, bool recursive);
int  mutex_lock(mutex_t *m);              /* 0 yoki SYNC_EDEADLK */
bool mutex_trylock(mutex_t *m);
void mutex_unlock(mutex_t *m);
bool mutex_owned(const mutex_t *m);       /* joriy vazifa egasimi */

/* Semafor */
void sem_init(semaphore_t *s, const char *name, int32_t initial);
int  sem_down(semaphore_t *s);            /* P / wait: 0 yoki SYNC_EINTR */
bool sem_trydown(semaphore_t *s);
void sem_up(semaphore_t *s);              /* V / signal */

/* Vazifa o'lganda (task.c): navbatlardan chiqarish, ushlab turgan mutexlarni bo'shatish */
void sync_task_died(task_t *t);

/* /proc/sync uchun hisobot */
int  sync_report(char *buf, uint32_t size);

#endif
