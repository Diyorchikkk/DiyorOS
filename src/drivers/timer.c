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
}

void timer_init(uint32_t hz)
{
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
    uint32_t target = ticks + (ms * frequency) / 1000;
    while (ticks < target)
        __asm__ volatile ("hlt");   /* keyingi uzilishgacha protsessorni dam oldiramiz */
}

void timer_on_second(void (*callback)(uint32_t seconds))
{
    second_callback = callback;
}
