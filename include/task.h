/*
 * task.h — ko'p vazifalilik (multitasking): vazifalar va scheduler.
 *
 * Bu bosqichdagi "vazifa" (task) — kernel oqimi (kernel thread):
 * har birining o'z steki va registrlari bor, lekin xotira umumiy
 * (bitta manzil maydoni). Alohida xotirali haqiqiy jarayonlar — v0.5 da.
 */
#ifndef TASK_H
#define TASK_H

#include <stdint.h>
#include <stdbool.h>
#include "cpu.h"

#define MAX_TASKS        16
#define TASK_STACK_SIZE  8192     /* har bir vazifaning kernel steki: 8 KB */
#define TASK_NAME_LEN    16
#define TASK_NOTE_LEN    32
#define SCHED_QUANTUM    5        /* vaqt kvanti: 5 tick = 50 ms */

typedef enum {
    TASK_UNUSED = 0,   /* jadvaldagi bo'sh joy */
    TASK_READY,        /* ishlashga tayyor, navbat kutmoqda */
    TASK_RUNNING,      /* hozir protsessorda ishlayapti */
    TASK_SLEEPING,     /* ma'lum vaqtgacha uxlayapti */
    TASK_BLOCKED,      /* hodisani kutmoqda (masalan, tugma bosilishini) */
    TASK_ZOMBIE        /* tugagan, resurslari hali bo'shatilmagan */
} task_state_t;

typedef void (*task_entry_t)(uint32_t arg);

typedef struct {
    uint32_t     pid;
    char         name[TASK_NAME_LEN];
    task_state_t state;
    registers_t *regs;          /* to'xtatilgan paytdagi registrlar (stekda saqlangan) */
    uint8_t     *stack;         /* kmalloc bilan ajratilgan stek (shell uchun 0) */
    task_entry_t entry;
    uint32_t     arg;
    uint32_t     wake_tick;     /* SLEEPING bo'lsa — qachon uyg'onadi */
    uint32_t     cpu_ticks;     /* jami qancha tick ishlagan */
    uint32_t     cpu_prev;      /* oldingi soniyadagi qiymat (CPU% hisoblash uchun) */
    uint32_t     cpu_percent;   /* oxirgi soniyadagi protsessor ulushi */
    uint32_t     switches;      /* necha marta protsessorga chiqarilgan */
    char         note[TASK_NOTE_LEN];   /* vazifa o'zi yozadigan qisqa holat */
} task_t;

/* Ishga tushirish: joriy oqim (shell) 1-vazifaga aylanadi, idle yaratiladi */
void     sched_init(void);
bool     sched_active(void);

/* Vazifa yaratish. PID qaytaradi, 0 — joy yoki xotira yetmadi */
uint32_t task_create(const char *name, task_entry_t entry, uint32_t arg);
bool     task_kill(uint32_t pid);          /* false — bunday vazifa yo'q yoki o'ldirib bo'lmaydi */
void     task_exit(void);                  /* joriy vazifani tugatish */
void     task_reap(void);                  /* tugagan vazifalar xotirasini bo'shatish */

void     task_yield(void);                 /* navbatni ixtiyoriy ravishda boshqaga berish */
void     task_sleep_ticks(uint32_t ticks);
void     task_block(void);                 /* joriy vazifani BLOCKED qilib, navbatni berish */
void     task_unblock(task_t *t);          /* uzilish ichidan chaqirish mumkin */

task_t  *task_current(void);
task_t  *task_get(int index);              /* jadvaldagi i-yozuv (ps uchun) */
uint32_t sched_switch_count(void);

/* Uzilishlar bilan bog'liq (timer.c va idt.c chaqiradi) */
void         sched_tick(void);
registers_t *sched_maybe_switch(registers_t *r);
registers_t *sched_yield_handler(registers_t *r);

#endif
