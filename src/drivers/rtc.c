/*
 * rtc.c — Real Time Clock (CMOS soati).
 *
 * Kompyuter o'chiq bo'lsa ham, anakartadagi batareya bilan ishlaydigan
 * CMOS chipi sana va vaqtni saqlaydi. Unga 0x70 (registr tanlash)
 * va 0x71 (qiymat o'qish) portlari orqali murojaat qilinadi.
 * Qiymatlar ko'pincha BCD formatida bo'ladi (masalan 0x45 = 45).
 */
#include <stdint.h>
#include "drivers.h"
#include "io.h"

static uint8_t cmos_read(uint8_t reg)
{
    outb(0x70, reg);
    return inb(0x71);
}

static uint8_t bcd_to_bin(uint8_t v)
{
    return (v & 0x0F) + ((v >> 4) * 10);
}

void rtc_read(rtc_time_t *t)
{
    while (cmos_read(0x0A) & 0x80)   /* soat yangilanayotgan bo'lsa kutamiz */
        ;

    uint8_t second = cmos_read(0x00);
    uint8_t minute = cmos_read(0x02);
    uint8_t hour   = cmos_read(0x04);
    uint8_t day    = cmos_read(0x07);
    uint8_t month  = cmos_read(0x08);
    uint8_t year   = cmos_read(0x09);
    uint8_t reg_b  = cmos_read(0x0B);

    if (!(reg_b & 0x04)) {           /* BCD formatdan oddiy songa */
        second = bcd_to_bin(second);
        minute = bcd_to_bin(minute);
        hour   = bcd_to_bin(hour & 0x7F) | (hour & 0x80);
        day    = bcd_to_bin(day);
        month  = bcd_to_bin(month);
        year   = bcd_to_bin(year);
    }
    if (!(reg_b & 0x02) && (hour & 0x80))   /* 12 soatlik formatdan 24 ga */
        hour = ((hour & 0x7F) + 12) % 24;

    t->second = second;
    t->minute = minute;
    t->hour   = hour;
    t->day    = day;
    t->month  = month;
    t->year   = 2000 + year;
}
