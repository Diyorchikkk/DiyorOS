/* drivers.h — taymer, klaviatura va real vaqt soati drayverlari */
#ifndef DRIVERS_H
#define DRIVERS_H

#include <stdint.h>

/* --- Taymer (PIT, IRQ0) --- */
void     timer_init(uint32_t hz);
uint32_t timer_ticks(void);
uint32_t timer_seconds(void);
void     timer_sleep_ms(uint32_t ms);
void     timer_on_second(void (*callback)(uint32_t seconds));

/* --- Klaviatura (PS/2, IRQ1) --- */
#define KEY_UP   0x11   /* yuqori strelka uchun maxsus kod */
#define KEY_DOWN 0x12   /* pastki strelka */
#define KEY_PGUP 0x13   /* Page Up   (Mac: Fn + yuqori strelka) */
#define KEY_PGDN 0x14   /* Page Down (Mac: Fn + pastki strelka) */
#define KEY_SCROLL_UP   0x15   /* Shift + yuqori strelka yoki sichqoncha g'ildiragi */
#define KEY_SCROLL_DOWN 0x16
#define KEY_LEFT   0x17
#define KEY_RIGHT  0x18
#define KEY_HOME   0x19 /* Mac: Fn + chap strelka */
#define KEY_END    0x1A /* Mac: Fn + o'ng strelka */
#define KEY_ESC    0x1B
#define KEY_DELETE 0x1C /* Mac: Fn + Backspace */
void keyboard_init(void);
char keyboard_getchar(void);   /* tugma bosilguncha kutadi */
void keyboard_inject(char c);  /* boshqa drayver (sichqoncha) buferga belgi qo'shishi uchun */
void keyboard_cancel_waiter(void *task);   /* tugagan vazifa tugmani endi kutmaydi */
#define KEY_CTRL_C 0x03

/* --- Sichqoncha (PS/2, IRQ12) — hozircha faqat g'ildirak --- */
int  mouse_init(void);         /* 1 — g'ildirakli sichqoncha topildi */

/* --- Real vaqt soati (CMOS RTC) --- */
typedef struct {
    uint8_t  second, minute, hour, day, month;
    uint16_t year;
} rtc_time_t;
void rtc_read(rtc_time_t *t);

#endif
