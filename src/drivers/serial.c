/*
 * serial.c — serial port (COM1, UART 16550) drayveri (v0.6.1).
 *
 * Serial port — eng sodda aloqa kanali: baytlar birma-bir sim orqali
 * yuboriladi. Haqiqiy operatsion tizimlarda u "konsol" sifatida ishlatiladi:
 * ekran ishlamay qolsa ham (masalan, kernel panic paytida) xabarlar serial
 * portdan chiqaveradi va boshqa kompyuterda yozib olinadi.
 *
 * QEMU da "-serial stdio" bilan ishga tushirilsa, DiyorOS ekraniga chiqqan
 * hamma narsa va kernel jurnali Mac terminalida ham ko'rinadi.
 *
 * Portlar (COM1 = 0x3F8):
 *   +0  ma'lumot (yozish: yuborish, o'qish: qabul qilish) / DLAB=1 da bo'luvchi (past)
 *   +1  uzilishlarni yoqish                               / DLAB=1 da bo'luvchi (yuqori)
 *   +2  FIFO boshqaruvi
 *   +3  liniya boshqaruvi (bit 7 = DLAB)
 *   +4  modem boshqaruvi
 *   +5  liniya holati (bit 5: yuborish buferi bo'sh)
 */
#include "klog.h"
#include "io.h"

#define COM1 0x3F8
#define TX_TIMEOUT 100000

static bool present = false;

void serial_init(void)
{
    outb(COM1 + 1, 0x00);        /* uzilishlar kerak emas — o'zimiz so'raymiz (polling) */
    outb(COM1 + 3, 0x80);        /* DLAB = 1: tezlik bo'luvchisini yozish rejimi */
    outb(COM1 + 0, 0x01);        /* bo'luvchi = 1 -> 115200 bod */
    outb(COM1 + 1, 0x00);
    outb(COM1 + 3, 0x03);        /* 8 bit, juftlik yo'q, 1 stop bit (8N1) */
    outb(COM1 + 2, 0xC7);        /* FIFO yoqish va tozalash */

    /* Port haqiqatan bormi? "Loopback" rejimida yuborilgan bayt o'zimizga qaytishi kerak.
       Haqiqiy kompyuterda COM1 bo'lmasa, bu sinov o'tmaydi va serial o'chiriladi. */
    outb(COM1 + 4, 0x1E);
    outb(COM1 + 0, 0xAE);
    if (inb(COM1 + 0) != 0xAE) {
        present = false;
        return;
    }
    outb(COM1 + 4, 0x0F);        /* oddiy ish rejimi */
    present = true;
}

bool serial_present(void)
{
    return present;
}

void serial_putc(char c)
{
    if (!present)
        return;
    if (c == '\n')
        serial_putc('\r');       /* terminallar "\r\n" kutadi */
    for (int i = 0; i < TX_TIMEOUT; i++)
        if (inb(COM1 + 5) & 0x20) {
            outb(COM1, (uint8_t)c);
            return;
        }
    /* Kutish vaqti tugadi: port javob bermayapti — tizimni osib qo'ymaymiz */
}

void serial_puts(const char *s)
{
    while (*s)
        serial_putc(*s++);
}
