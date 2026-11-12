/*
 * task.h — ko'p vazifalilik (multitasking): vazifalar va scheduler.
 *
 * Ikki xil vazifa bor:
 *   - kernel oqimi (v0.3): ring 0 da ishlaydi, xotira kernel bilan umumiy
 *     (shell, idle, spinner, primes, busy);
 *   - user jarayon (v0.5): ring 3 da ishlaydi, o'z Page Directory si
 *     (alohida xotirasi) bor, kernelga faqat syscall orqali murojaat qiladi
 *     (/bin dagi dasturlar).
 */
#ifndef TASK_H
#define TASK_H

#include <stdint.h>
#include <stdbool.h>
#include "cpu.h"

#define MAX_TASKS        16
#define TASK_STACK_SIZE  16384    /* har bir vazifaning kernel steki: 16 KB */
#define STACK_CANARY     0x57ACC0DEu   /* stek tubidagi "qo'riqchi" qiymat (v0.6.1) */
#define STACK_RED_ZONE   1024     /* stek tubiga shuncha qolsa — "deyarli to'ldi" (panic) */
#define FPU_STATE_SIZE   108      /* fnsave saqlaydigan FPU holati hajmi */
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

    /* v0.5: user jarayonlar */
    bool         user;          /* true — ring 3 dagi dastur */
    uint32_t    *page_dir;      /* o'z Page Directory si (kernel oqimlari uchun 0) */
    uint32_t     brk_start;     /* dastur heap i (sbrk) boshlanishi */
    uint32_t     brk;           /* heap ning joriy oxiri */
    uint32_t     user_pages;    /* jarayon egallagan sahifalar soni */
    uint32_t     parent_pid;
    uint32_t     wait_pid;      /* qaysi jarayon tugashini kutyapti */
    int32_t      exit_code;

    /* v0.6.1 */
    uint32_t    *stack_guard;   /* stekning eng tubi: u yerda STACK_CANARY turishi shart */
    bool         fpu_used;      /* dastur FPU (suzuvchi nuqta) ishlatganmi */
    uint8_t      fpu_state[FPU_STATE_SIZE];   /* boshqa dastur FPU ni olganda saqlangan holat */
} task_t;

#define EXIT_KILLED   (-9)      /* kill buyrug'i bilan */
#define EXIT_CTRL_C   (-2)      /* Ctrl+C bilan */
#define EXIT_FAULT    (-11)     /* xato (Segmentation fault) */
#define EXIT_TTIN     (-21)     /* fon jarayoni klaviaturadan o'qimoqchi bo'ldi */

/* Ishga tushirish: joriy oqim (shell) 1-vazifaga aylanadi, idle yaratiladi */
void     sched_init(void);
bool     sched_active(void);

/* Vazifa yaratish. PID qaytaradi, 0 — joy yoki xotira yetmadi */
uint32_t task_create(const char *name, task_entry_t entry, uint32_t arg);
bool     task_kill(uint32_t pid);          /* false — bunday vazifa yo'q yoki o'ldirib bo'lmaydi */
void     task_exit(void);                  /* joriy vazifani tugatish (kod 0) */
void     task_exit_code(int32_t code);

/* v0.5: user jarayonlar */
uint32_t task_create_user(const char *name, uint32_t *page_dir, uint32_t entry,
                          uint32_t user_esp, uint32_t brk, uint32_t pages);
int32_t  task_wait(uint32_t pid);          /* jarayon tugashini kutish, chiqish kodini qaytaradi */
void     task_signal_kill(uint32_t pid, int32_t code);   /* uzilish ichidan ham (Ctrl+C) */
task_t  *task_find(uint32_t pid);
void     task_set_foreground(uint32_t pid);
uint32_t task_foreground(void);
void     task_reap(void);                  /* tugagan vazifalar xotirasini bo'shatish */
void     task_reap_pid(uint32_t pid);

void     task_yield(void);                 /* navbatni ixtiyoriy ravishda boshqaga berish */
void     task_sleep_ticks(uint32_t ticks);
void     task_block(void);                 /* joriy vazifani BLOCKED qilib, navbatni berish */
void     task_unblock(task_t *t);          /* uzilish ichidan chaqirish mumkin */

task_t  *task_current(void);
task_t  *task_get(int index);              /* jadvaldagi i-yozuv (ps uchun) */
uint32_t sched_switch_count(void);

/* FPU (v0.6.1): "lazy" kontekst almashtirish — task.c da tushuntirilgan */
void     fpu_init(void);
bool     fpu_handle_nm(registers_t *r);    /* exception #7; false — ishlov berilmadi */

/* Uzilishlar bilan bog'liq (timer.c va idt.c chaqiradi) */
void         sched_tick(void);
registers_t *sched_maybe_switch(registers_t *r);
registers_t *sched_yield_handler(registers_t *r);

#endif
