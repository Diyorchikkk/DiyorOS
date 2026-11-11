/*
 * mouse.c — PS/2 sichqoncha drayveri (hozircha faqat g'ildirak).
 *
 * Sichqoncha klaviatura bilan bir xil kontrollerga (8042) ulangan.
 * Unga buyruq yuborish uchun avval 0x64 portiga 0xD4 yoziladi
 * ("keyingi bayt sichqonchaga"), keyin buyruq 0x60 portiga yoziladi.
 *
 * Oddiy sichqoncha 3 baytlik paket yuboradi (tugmalar, X, Y).
 * G'ildirakni yoqish uchun maxsus "sehrli ketma-ketlik" bor:
 * namuna tezligini 200, 100, 80 qilib o'rnatsak, sichqoncha o'zini
 * "IntelliMouse" (ID = 3) deb tanitadi va 4 baytlik paket yubora
 * boshlaydi: 4-baytda g'ildirak qancha aylangani (Z) bo'ladi.
 *
 * G'ildirak aylansa, klaviatura buferiga KEY_SCROLL_UP/DOWN kodlarini
 * qo'shamiz — shell ularni klaviatura tugmalari kabi qabul qiladi.
 */
#include <stdint.h>
#include "drivers.h"
#include "cpu.h"
#include "io.h"

#define PS2_DATA    0x60
#define PS2_STATUS  0x64
#define PS2_COMMAND 0x64
#define LINES_PER_NOTCH 3      /* g'ildirakning bir "qadami" — 3 qator */

static uint8_t packet[4];
static int packet_index = 0;
static int packet_size = 3;

/* Kontrollerga yozish mumkin bo'lguncha kutish */
static void wait_write(void)
{
    for (int t = 0; t < 100000; t++)
        if (!(inb(PS2_STATUS) & 0x02))
            return;
}

/* Kontrollerdan o'qish mumkin bo'lguncha kutish */
static void wait_read(void)
{
    for (int t = 0; t < 100000; t++)
        if (inb(PS2_STATUS) & 0x01)
            return;
}

static uint8_t mouse_read(void)
{
    wait_read();
    return inb(PS2_DATA);
}

/* Sichqonchaga buyruq yuborish va javobini (ACK = 0xFA) o'qish */
static void mouse_write(uint8_t value)
{
    wait_write();
    outb(PS2_COMMAND, 0xD4);
    wait_write();
    outb(PS2_DATA, value);
    mouse_read();
}

static void set_sample_rate(uint8_t rate)
{
    mouse_write(0xF3);
    mouse_write(rate);
}

static void mouse_handler(registers_t *r)
{
    (void)r;
    uint8_t b = inb(PS2_DATA);

    /* Paket boshida kelgan 0xFA — kechikkan "tasdiq" (ACK) bayti, paket emas */
    if (packet_index == 0 && b == 0xFA)
        return;

    /* Birinchi baytning 3-biti doim 1. Bu paket boshini topishga yordam beradi */
    if (packet_index == 0 && !(b & 0x08))
        return;

    packet[packet_index++] = b;
    if (packet_index < packet_size)
        return;
    packet_index = 0;

    if (packet_size == 4) {
        int8_t z = (int8_t)(packet[3] & 0x0F);
        if (z & 0x08)
            z |= (int8_t)0xF0;            /* 4-bitli manfiy sonni kengaytirish */
        char key = z < 0 ? KEY_SCROLL_UP : KEY_SCROLL_DOWN;
        if (z != 0)
            for (int i = 0; i < LINES_PER_NOTCH; i++)
                keyboard_inject(key);
    }
}

int mouse_init(void)
{
    /* Ikkinchi PS/2 portini (sichqoncha) yoqish */
    wait_write();
    outb(PS2_COMMAND, 0xA8);

    /* Kontroller sozlamasida IRQ12 ni yoqish */
    wait_write();
    outb(PS2_COMMAND, 0x20);
    uint8_t config = mouse_read();
    config |= 0x02;          /* sichqoncha uzilishi yoqilgan */
    config &= ~0x20;         /* sichqoncha taktini o'chirmaslik */
    wait_write();
    outb(PS2_COMMAND, 0x60);
    wait_write();
    outb(PS2_DATA, config);

    mouse_write(0xF6);       /* standart sozlamalar */

    /* G'ildirakni yoqish uchun sehrli ketma-ketlik */
    set_sample_rate(200);
    set_sample_rate(100);
    set_sample_rate(80);
    mouse_write(0xF2);       /* "ID ingni ayt" */
    uint8_t id = mouse_read();
    packet_size = (id == 3 || id == 4) ? 4 : 3;

    mouse_write(0xF4);       /* ma'lumot yuborishni boshlash */

    /* Kontrollerda qolib ketgan baytlarni tozalash */
    while (inb(PS2_STATUS) & 0x01)
        inb(PS2_DATA);

    irq_register(12, mouse_handler);
    return packet_size == 4;
}
