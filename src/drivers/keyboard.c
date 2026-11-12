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
 */
#include <stdint.h>
#include <stdbool.h>
#include "drivers.h"
#include "cpu.h"
#include "io.h"
#include "task.h"
#include "irq.h"

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

static task_t *waiter = 0;       /* tugma kutib BLOCKED bo'lgan vazifa */

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
    if (waiter) {                /* kutayotgan vazifani uyg'otamiz */
        task_unblock(waiter);
        waiter = 0;
    }
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
        if (sc == 0x48) buffer_push(shift ? KEY_SCROLL_UP : KEY_UP);
        else if (sc == 0x50) buffer_push(shift ? KEY_SCROLL_DOWN : KEY_DOWN);
        else if (sc == 0x4B) buffer_push(KEY_LEFT);
        else if (sc == 0x4D) buffer_push(KEY_RIGHT);
        else if (sc == 0x47) buffer_push(KEY_HOME);
        else if (sc == 0x4F) buffer_push(KEY_END);
        else if (sc == 0x53) buffer_push(KEY_DELETE);
        else if (sc == 0x49) buffer_push(KEY_PGUP);
        else if (sc == 0x51) buffer_push(KEY_PGDN);
        else if (sc == 0x1C) buffer_push('\n');   /* numpad Enter */
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
        uint32_t fg = task_foreground();
        if (fg)
            task_signal_kill(fg, EXIT_CTRL_C);   /* oldingi plandagi dasturni to'xtatish */
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
            buffer_push(code);
        return;                                  /* boshqa Ctrl+tugma — e'tiborsiz */
    }

    char c = shift ? keymap_shift[sc] : keymap[sc];
    if (caps_lock) {                       /* Caps Lock faqat harflarga ta'sir qiladi */
        if (c >= 'a' && c <= 'z') c -= 32;
        else if (c >= 'A' && c <= 'Z') c += 32;
    }
    if (c)
        buffer_push(c);
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
            irq_restore(flags);          /* avvalgi holatga (syscall ichida — o'chiq qoladi) */
            return c;
        }
        if (sched_active()) {
            waiter = task_current();
            task_block();
        } else {
            __asm__ volatile ("sti; hlt");
        }
    }
}

void keyboard_cancel_waiter(void *task)
{
    if (waiter == task)
        waiter = 0;
}
