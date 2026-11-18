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
#define TASK_MAX_FDS     16       /* v0.10: bitta jarayonning deskriptor jadvali */
#define TASK_MAX_CHILDREN 4       /* bitta jarayonning bir vaqtdagi bolalari ("fork bomb" ga qarshi) */

/* v0.11: rejalashtirish algoritmlari (sched buyrug'i bilan almashtiriladi) */
typedef enum {
    SCHED_RR = 0,        /* Round-Robin: navbat bilan, teng kvant */
    SCHED_PRIO,          /* qat'iy ustuvorlik: eng yuqorisi doim birinchi */
    SCHED_MLFQ,          /* ko'p darajali navbat: CPU ni ko'p yeganlar pastga tushadi */
    SCHED_LOTTERY,       /* lotereya: ustuvorlik — chiptalar soni, g'olib tasodifiy */
    SCHED_POLICY_COUNT
} sched_policy_t;

#define PRIO_MIN      1           /* ustuvorlik: 1 (past) ... 10 (tizim) */
#define PRIO_DEFAULT  5           /* user dasturlar */
#define PRIO_SYSTEM   10          /* kernel oqimlari: shell, oyna menejeri, konsol */
#define MLFQ_LEVELS   3

typedef enum {
    TASK_UNUSED = 0,   /* jadvaldagi bo'sh joy */
    TASK_READY,        /* ishlashga tayyor, navbat kutmoqda */
    TASK_RUNNING,      /* hozir protsessorda ishlayapti */
    TASK_SLEEPING,     /* ma'lum vaqtgacha uxlayapti */
    TASK_BLOCKED,      /* hodisani kutmoqda (masalan, tugma bosilishini) */
    TASK_ZOMBIE        /* tugagan, resurslari hali bo'shatilmagan */
} task_state_t;

typedef void (*task_entry_t)(uint32_t arg);

struct wait_queue;
struct mutex;
struct vfs_node;

typedef struct task {
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

    /* v0.8: sinxronizatsiya */
    struct task       *wq_next;       /* kutish navbatidagi keyingi vazifa */
    struct wait_queue *wait_queue;    /* qaysi navbatda kutyapti (0 — hech qaysi) */
    const char        *wait_reason;   /* nimani kutyapti: "mutex", "pipe", ... (ps uchun) */
    struct mutex      *waiting_mutex; /* qaysi mutexni kutyapti (deadlock aniqlash uchun) */

    /* v0.8: signal — xavfsiz to'xtatish */
    volatile bool      kill_pending;  /* to'xtatish so'ralgan: user rejimga qaytishda tugaydi */
    int32_t            kill_code;

    /* v0.8: jarayon guruhi */
    uint32_t           pgid;          /* quvurdagi (a | b | c) barcha jarayonlar bitta guruhda */
    uint8_t            color;         /* ekranga yozish rangi (setcolor) — har jarayonniki o'ziniki */
    bool               out_failed;    /* yo'naltirilgan stdout ga yozishda xato bo'lgan (xabar bir marta) */

    /* v0.10: har jarayonning O'Z deskriptor jadvali va joriy papkasi */
    int16_t            fd[TASK_MAX_FDS];   /* fd -> ochiq fayl yozuvi (vfs.h: FD_FREE, FD_CONSOLE) */
    struct vfs_node   *cwd;               /* nisbiy yo'llar shu papkadan hisoblanadi */
    uint32_t           forks;             /* fork() orqali yaratilgan (ps: IZOH) */

    /* v0.11: rejalashtirish va o'lchovlar */
    uint8_t            prio;              /* ustuvorlik 1..10 (PRIO, LOTTERY) */
    uint8_t            level;             /* MLFQ darajasi: 0 — yuqori */
    uint16_t           level_used;        /* joriy darajada ishlatgan tick lari */
    uint32_t           start_tick;        /* yaratilgan payt */
    uint32_t           first_run_tick;    /* birinchi marta protsessorga chiqqan payt */
    uint32_t           end_tick;          /* tugagan payt */
    bool               has_run;
    bool               woke;              /* READY ga uyg'onib o'tdi (uxlash/kutishdan) */
    uint64_t           ready_since;       /* TSC: qachondan beri navbatda */
    uint64_t           ready_wait;        /* jami navbatda kutgan taktlar */
    uint32_t           wakeups;           /* necha marta uyg'ongan */
    uint64_t           wake_lat_sum;      /* uyg'onishdan protsessorga chiqqungacha (taktlar) */
    uint64_t           wake_lat_max;
    uint32_t           seq;               /* navbatga turgan tartib raqami: kichigi — oldinroq (FIFO) */
    uint64_t           start_tsc, end_tsc;   /* yaratilgan va tugagan payt (TSC) */
    uint64_t           first_wait;        /* kelishdan birinchi ishga tushishgacha (taktlar) */
    uint64_t           run_cycles;        /* protsessorda ishlagan jami taktlar (aniq CPU vaqti) */
    uint64_t           run_since;         /* joriy ishlash boshlangan payt */
} task_t;

#define EXIT_KILLED   (-9)      /* kill buyrug'i bilan */
#define EXIT_CTRL_C   (-2)      /* Ctrl+C bilan */
#define EXIT_FAULT    (-11)     /* xato (Segmentation fault) */
#define EXIT_TTIN     (-21)     /* fon jarayoni klaviaturadan o'qimoqchi bo'ldi */
#define EXIT_PIPE     (-13)     /* o'quvchisi yo'q pipe ga yozdi (Unix: SIGPIPE) */

/* Ishga tushirish: joriy oqim (shell) 1-vazifaga aylanadi, idle yaratiladi */
void     sched_init(void);
bool     sched_active(void);

/* Vazifa yaratish. PID qaytaradi, 0 — joy yoki xotira yetmadi */
uint32_t task_create(const char *name, task_entry_t entry, uint32_t arg);
bool     task_kill(uint32_t pid);          /* false — bunday vazifa yo'q yoki o'ldirib bo'lmaydi */
void     task_exit(void);                  /* joriy vazifani tugatish (kod 0) */
void     task_exit_code(int32_t code);

/* v0.5: user jarayonlar */
/* v0.8: yangi jarayonning stdin/stdout va guruhi.
   v0.10: fd_in / fd_out — CHAQIRUVCHINING deskriptor raqamlari (-1 — uning fd 0 / fd 1 i
   meros). Bola ularning nusxasini oladi; chaqiruvchi o'zinikini o'zi yopadi */
typedef struct {
    int      fd_in;
    int      fd_out;
    uint32_t pgid;
    bool     foreground;    /* yaratilishi bilan oldingi planga (klaviatura, Ctrl+C) */
} spawn_opts_t;

uint32_t task_create_user(const char *name, uint32_t *page_dir, uint32_t entry,
                          uint32_t user_esp, uint32_t brk, uint32_t pages,
                          const spawn_opts_t *opts);
int32_t  task_wait(uint32_t pid);          /* jarayon tugashini kutish, chiqish kodini qaytaradi */
int32_t  task_wait_child(int32_t pid, int32_t *code);   /* v0.10: pid <= 0 — istalgan bola; bola PID */
int32_t  task_fork(registers_t *r);        /* v0.10: bola PID (otada), xato < 0 */
void     task_exec(registers_t *r, const char *name, uint32_t *dir, uint32_t entry,
                   uint32_t user_esp, uint32_t brk, uint32_t pages);
int      task_count_children(uint32_t pid);
void     task_signal_kill(uint32_t pid, int32_t code);   /* uzilish ichidan ham (Ctrl+C) */
void     task_signal_group(uint32_t pgid, int32_t code); /* guruhdagi barcha jarayonlarga (v0.8) */
bool     task_should_abort(void);          /* joriy user jarayonni to'xtatish so'ralganmi (v0.8) */
void     task_check_signals(registers_t *r);   /* user rejimga qaytishdan oldin (idt.c) */
task_t  *task_find(uint32_t pid);
void     task_set_foreground(uint32_t pid);
uint32_t task_foreground(void);
void     task_reap(void);                  /* tugagan vazifalar xotirasini bo'shatish */
void     task_reap_pid(uint32_t pid);

void     task_yield(void);                 /* navbatni ixtiyoriy ravishda boshqaga berish */
void     task_sleep_ticks(uint32_t ticks);
void     task_block(void);                 /* joriy vazifani BLOCKED qilib, navbatni berish */
void     task_unblock(task_t *t);
void     task_wake(task_t *t);          /* uxlayotgan yoki kutayotganni darhol uyg'otish (v0.7) */

task_t  *task_current(void);
task_t  *task_get(int index);              /* jadvaldagi i-yozuv (ps uchun) */
uint32_t sched_switch_count(void);

/* FPU (v0.6.1): "lazy" kontekst almashtirish — task.c da tushuntirilgan */
void     fpu_init(void);
bool     fpu_handle_nm(registers_t *r);    /* exception #7; false — ishlov berilmadi */

/* v0.11: algoritmni tanlash va sozlash */
void           sched_set_policy(sched_policy_t p);
sched_policy_t sched_policy(void);
const char    *sched_policy_name(sched_policy_t p);
void           sched_set_quantum(uint32_t ticks);    /* RR, PRIO, LOTTERY kvanti */
uint32_t       sched_quantum(void);
void           sched_mlfq_info(const uint32_t **quantum, const uint32_t **allot, uint32_t *boost);
bool           task_set_prio(uint32_t pid, int prio);
uint64_t       task_cpu_cycles(void);              /* joriy vazifaning aniq CPU vaqti (taktlar) */

/* Uzilishlar bilan bog'liq (timer.c va idt.c chaqiradi) */
void         sched_tick(void);
registers_t *sched_maybe_switch(registers_t *r);
registers_t *sched_yield_handler(registers_t *r);

#endif
