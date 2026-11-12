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
#include "irq.h"
#include <stdbool.h>

static uint8_t cmos_read(uint8_t reg)
{
    outb(0x70, reg);
    return inb(0x71);
}

static uint8_t bcd_to_bin(uint8_t v)
{
    return (uint8_t)((v & 0x0F) + ((v >> 4) * 10));
}

typedef struct { uint8_t second, minute, hour, day, month, year; } raw_time_t;

static void read_raw(raw_time_t *r)
{
    for (int i = 0; i < 100000 && (cmos_read(0x0A) & 0x80); i++)
        ;                            /* soat yangilanayotgan bo'lsa kutamiz (chegara bilan) */
    r->second = cmos_read(0x00);
    r->minute = cmos_read(0x02);
    r->hour   = cmos_read(0x04);
    r->day    = cmos_read(0x07);
    r->month  = cmos_read(0x08);
    r->year   = cmos_read(0x09);
}

void rtc_read(rtc_time_t *t)
{
    /*
     * O'qish paytida soniya almashib qolsa (masalan 10:59:59 -> 11:00:00),
     * yarmi eski, yarmi yangi qiymat chiqishi mumkin ("11:59:59"). Shuning
     * uchun ikki marta ketma-ket bir xil natija chiqquncha o'qiymiz.
     * 0x70 portiga yozish va 0x71 dan o'qish orasida boshqa vazifa aralashmasligi
     * uchun uzilishlar o'chiriladi.
     */
    uint32_t flags = irq_save();
    raw_time_t a, b;
    read_raw(&a);
    for (int i = 0; i < 5; i++) {
        read_raw(&b);
        if (a.second == b.second && a.minute == b.minute && a.hour == b.hour &&
            a.day == b.day && a.month == b.month && a.year == b.year)
            break;
        a = b;
    }
    uint8_t second = a.second, minute = a.minute, hour = a.hour;
    uint8_t day = a.day, month = a.month, year = a.year;
    uint8_t reg_b = cmos_read(0x0B);
    irq_restore(flags);

    if (!(reg_b & 0x04)) {           /* BCD formatdan oddiy songa */
        second = bcd_to_bin(second);
        minute = bcd_to_bin(minute);
        hour   = (uint8_t)(bcd_to_bin(hour & 0x7F) | (hour & 0x80));
        day    = bcd_to_bin(day);
        month  = bcd_to_bin(month);
        year   = bcd_to_bin(year);
    }
    if (!(reg_b & 0x02)) {           /* 12 soatlik formatdan 24 ga */
        bool pm = hour & 0x80;
        hour &= 0x7F;
        if (hour == 12)
            hour = 0;                /* 12 AM = 00, 12 PM = 12 */
        if (pm)
            hour += 12;
    }

    t->second = second;
    t->minute = minute;
    t->hour   = hour;
    t->day    = day;
    t->month  = month;
    t->year   = 2000 + year;
}
