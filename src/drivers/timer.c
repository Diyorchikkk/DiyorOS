/*
 * timer.c — PIT (Programmable Interval Timer, 8253/8254 chip).
 *
 * PIT 1 193 182 Hz chastotada ishlaydi. Unga "bo'luvchi" (divisor)
 * bersak, har divisor taktda IRQ0 uzilishini yuboradi.
 * Masalan 100 Hz uchun: 1193182 / 100 = 11931.
 * Har uzilishda "tick" hisoblagichini oshiramiz — bu tizim soati.
 * Kelajakda shu taymer jarayonlarni almashtirish (scheduler) uchun ishlatiladi.
 */
#include <stdint.h>
#include "drivers.h"
#include "cpu.h"
#include "io.h"
#include "task.h"
#include "tsc.h"

#define PIT_FREQUENCY 1193182
#define PIT_CHANNEL0  0x40
#define PIT_COMMAND   0x43

static volatile uint32_t ticks = 0;
static uint32_t frequency = 100;
static void (*second_callback)(uint32_t) = 0;

static void timer_handler(registers_t *r)
{
    (void)r;
    ticks++;
    if (ticks % frequency == 0 && second_callback)
        second_callback(ticks / frequency);
    sched_tick();                   /* scheduler: CPU hisobi, uyg'otish, vaqt kvanti */
}

void timer_init(uint32_t hz)
{
    if (hz < 19)
        hz = 19;                    /* bo'luvchi 16 bitga sig'ishi kerak (1193182 / 65535 = 18.2) */
    frequency = hz;
    uint32_t divisor = PIT_FREQUENCY / hz;

    outb(PIT_COMMAND, 0x36);                       /* kanal 0, rejim 3 (kvadrat to'lqin) */
    outb(PIT_CHANNEL0, (uint8_t)(divisor & 0xFF)); /* pastki bayt */
    outb(PIT_CHANNEL0, (uint8_t)(divisor >> 8));   /* yuqori bayt */

    irq_register(0, timer_handler);
}

uint32_t timer_ticks(void)
{
    return ticks;
}

uint32_t timer_seconds(void)
{
    return ticks / frequency;
}

void timer_sleep_ms(uint32_t ms)
{
    /* ms * frequency 32 bitdan oshib ketmasligi uchun ikki qismga bo'lib hisoblaymiz */
    uint32_t wait = (ms / 1000) * frequency + ((ms % 1000) * frequency + 999) / 1000;
    if (wait == 0)
        wait = 1;
    if (wait > 0x3FFFFFFF)
        wait = 0x3FFFFFFF;
    if (sched_active()) {           /* v0.3: vazifa uxlaydi, protsessor boshqalarga beriladi */
        task_sleep_ticks(wait);
        return;
    }
    uint32_t start = ticks;
    while (ticks - start < wait)
        __asm__ volatile ("hlt");   /* keyingi uzilishgacha protsessorni dam oldiramiz */
}

void timer_on_second(void (*callback)(uint32_t seconds))
{
    second_callback = callback;
}

/* ------------------------------ TSC (v0.11) ------------------------------ */

static uint32_t khz = 0;
static uint64_t tsc_boot = 0;

bool tsc_available(void)
{
    uint32_t a = 1, b, c = 0, d;
    __asm__ volatile ("cpuid" : "+a"(a), "=b"(b), "+c"(c), "=d"(d));
    return d & (1u << 4);                  /* CPUID.1:EDX bit 4 — TSC */
}

/* 5 ta taymer tikida (50 ms) TSC qanchaga o'sadi -> chastota */
void tsc_calibrate(void)
{
    if (!tsc_available())
        return;
    uint32_t t = ticks;
    while (ticks == t)                     /* tik chegarasini kutamiz — o'lchov aniqroq */
        __asm__ volatile ("hlt");
    uint64_t start = rdtsc();
    t = ticks;
    while (ticks - t < 5)
        __asm__ volatile ("hlt");
    uint64_t cycles = rdtsc() - start;
    khz = (uint32_t)udiv64(cycles, 5 * 1000 / frequency);   /* 50 ms -> 1 ms dagi taktlar */
    tsc_boot = start;
}

uint32_t tsc_khz(void) { return khz; }

uint32_t tsc_to_us(uint64_t cycles)
{
    if (khz < 1000)
        return 0;
    return (uint32_t)udiv64(cycles, khz / 1000);
}

uint32_t tsc_to_ns(uint64_t cycles)
{
    if (!khz)
        return 0;
    return (uint32_t)udiv64(cycles * 1000000, khz);
}

uint64_t tsc_now_us(void)
{
    if (khz < 1000)
        return (uint64_t)ticks * (1000000 / frequency);
    return udiv64(rdtsc() - tsc_boot, khz / 1000);
}
