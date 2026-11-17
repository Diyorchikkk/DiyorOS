/*
 * keyboard.c — PS/2 klaviatura drayveri.
 *
 * Tugma bosilganda (yoki qo'yib yuborilganda) klaviatura IRQ1 uzilishini
 * yuboradi. 0x60 portdan "scancode" ni o'qiymiz — bu tugmaning raqami,
 * harf emas. Scancode ni jadval orqali belgiga aylantiramiz.
 * Qo'yib yuborilganda scancode ga 0x80 qo'shilgan bo'ladi.
 *
 * Belgilar aylanma buferga (ring buffer) yoziladi, shell esa
 * keyboard_getchar() orqali ulardan o'qiydi. Bu "producer–consumer"
 * modelining oddiy misoli: uzilish yozadi, shell o'qiydi.
 *
 * v0.7: grafik rejimda tugma avval oyna menejeriga beriladi (key_hook).
 * Faol oyna terminal bo'lmasa (masalan, o'yin), tugma o'sha oynaga ketadi va
 * shell uni ko'rmaydi.
 */
#include <stdint.h>
#include <stdbool.h>
#include "drivers.h"
#include "cpu.h"
#include "io.h"
#include "task.h"
#include "irq.h"
#include "sync.h"

#define KBD_DATA_PORT 0x60
#define BUFFER_SIZE   256

/* Scancode Set 1, US klaviatura — oddiy holat */
static const char keymap[128] = {
    0,   27,  '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', '\b',
    '\t','q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', '\n',
    0,   'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '`',
    0,   '\\','z', 'x', 'c', 'v', 'b', 'n', 'm', ',', '.', '/', 0,
    '*', 0,   ' '
};

/* Shift bosilgan holat */
static const char keymap_shift[128] = {
    0,   27,  '!', '@', '#', '$', '%', '^', '&', '*', '(', ')', '_', '+', '\b',
    '\t','Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', '{', '}', '\n',
    0,   'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', ':', '"', '~',
    0,   '|', 'Z', 'X', 'C', 'V', 'B', 'N', 'M', '<', '>', '?', 0,
    '*', 0,   ' '
};

static volatile char buffer[BUFFER_SIZE];
static volatile uint32_t head = 0, tail = 0;   /* head — yozish, tail — o'qish */

/* Tugma kutayotgan vazifalar. v0.8: bitta ko'rsatkich o'rniga kutish navbati —
   bir guruhdagi ikki dastur (ota va bolasi) bir vaqtda kutsa ham hech biri yo'qolmaydi */
static wait_queue_t waiters;

static key_hook_t key_hook = 0;

static bool shift = false;
static bool ctrl = false;
static bool caps_lock = false;
static bool extended = false;   /* 0xE0 prefiksi (strelkalar va h.k.) */

static void buffer_push(char c)
{
    uint32_t next = (head + 1) % BUFFER_SIZE;
    if (next != tail) {          /* bufer to'lgan bo'lsa, belgini tashlaymiz */
        buffer[head] = c;
        head = next;
    }
    wq_wake_all(&waiters);       /* kutayotganlarni uyg'otamiz: birinchi ulgurgani oladi */
}

/* Klaviaturadan kelgan tugma: avval ilgakka, u olmasa — buferga */
static void deliver(char c)
{
    if (key_hook && key_hook(c))
        return;
    buffer_push(c);
}

void keyboard_set_hook(key_hook_t hook)
{
    key_hook = hook;
}

static void keyboard_handler(registers_t *r)
{
    (void)r;
    uint8_t sc = inb(KBD_DATA_PORT);

    if (sc == 0xE0) {            /* kengaytirilgan tugma: keyingi bayt muhim */
        extended = true;
        return;
    }

    bool released = sc & 0x80;
    sc &= 0x7F;

    if (extended) {
        extended = false;
        if (sc == 0x1D) {                  /* o'ng Ctrl */
            ctrl = !released;
            return;
        }
        if (released)
            return;
        if (sc == 0x48) deliver(shift ? KEY_SCROLL_UP : KEY_UP);
        else if (sc == 0x50) deliver(shift ? KEY_SCROLL_DOWN : KEY_DOWN);
        else if (sc == 0x4B) deliver(KEY_LEFT);
        else if (sc == 0x4D) deliver(KEY_RIGHT);
        else if (sc == 0x47) deliver(KEY_HOME);
        else if (sc == 0x4F) deliver(KEY_END);
        else if (sc == 0x53) deliver(KEY_DELETE);
        else if (sc == 0x49) deliver(KEY_PGUP);
        else if (sc == 0x51) deliver(KEY_PGDN);
        else if (sc == 0x1C) deliver('\n');   /* numpad Enter */
        return;
    }

    if (sc == 0x2A || sc == 0x36) {        /* chap yoki o'ng Shift */
        shift = !released;
        return;
    }
    if (sc == 0x1D) {                      /* Ctrl */
        ctrl = !released;
        return;
    }
    if (released)
        return;
    if (sc == 0x3A) {                      /* Caps Lock */
        caps_lock = !caps_lock;
        return;
    }

    if (ctrl && sc == 0x2E) {              /* Ctrl+C */
        if (key_hook && key_hook(KEY_CTRL_C))
            return;                              /* grafik oyna (o'yin) uchun — dastur o'ldirilmaydi */
        uint32_t fg = task_foreground();
        if (fg)
            task_signal_group(fg, EXIT_CTRL_C);  /* oldingi plandagi dastur(lar)ni to'xtatish: a | b | c */
        else
            buffer_push(KEY_CTRL_C);             /* shell: kiritilayotgan qatorni bekor qilish */
        return;
    }
    if (ctrl) {                            /* terminallardagi odatiy Ctrl birikmalari (v0.6.1) */
        char code = 0;
        switch (keymap[sc]) {
        case 'a': code = KEY_HOME;   break;      /* qator boshiga */
        case 'e': code = KEY_END;    break;      /* qator oxiriga */
        case 'd': code = KEY_DELETE; break;      /* kursor ostidagi belgini o'chirish */
        case 'k': code = KEY_CTRL_K; break;      /* kursordan oxirigacha o'chirish */
        case 'u': code = KEY_CTRL_U; break;      /* butun qatorni o'chirish */
        case 'l': code = KEY_CTRL_L; break;      /* ekranni tozalash */
        }
        if (code)
            deliver(code);
        return;                                  /* boshqa Ctrl+tugma — e'tiborsiz */
    }

    char c = shift ? keymap_shift[sc] : keymap[sc];
    if (caps_lock) {                       /* Caps Lock faqat harflarga ta'sir qiladi */
        if (c >= 'a' && c <= 'z') c -= 32;
        else if (c >= 'A' && c <= 'Z') c += 32;
    }
    if (c)
        deliver(c);
}

void keyboard_init(void)
{
    while (inb(0x64) & 1)        /* oldin qolgan ma'lumotlarni tozalash */
        inb(KBD_DATA_PORT);
    irq_register(1, keyboard_handler);
}

void keyboard_inject(char c)
{
    buffer_push(c);
}

/*
 * Tugma bosilguncha kutish.
 * v0.3 dan: kutayotgan vazifa BLOCKED holatga o'tadi va protsessorni
 * boshqa vazifalarga beradi. Tugma bosilganda uzilish uni uyg'otadi.
 */
char keyboard_getchar(void)
{
    uint32_t flags = irq_save();
    for (;;) {
        __asm__ volatile ("cli");
        if (head != tail) {
            char c = buffer[tail];
            tail = (tail + 1) % BUFFER_SIZE;
            irq_restore(flags);
            return c;
        }
        if (sched_active()) {
            /* v0.8: uzilishli kutish — jarayonga signal (Ctrl+C, kill) kelsa, 0 qaytadi */
            if (wq_wait(&waiters, "klaviatura", true) == SYNC_EINTR) {
                irq_restore(flags);
                return 0;
            }
        } else {
            __asm__ volatile ("sti; hlt");
        }
    }
}


