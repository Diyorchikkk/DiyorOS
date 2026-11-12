/*
 * klog.c — kernel jurnali (v0.6.1). Linux dagi printk + dmesg ning sodda ko'rinishi.
 *
 * Kernel ichida nima bo'layotganini bilish uchun xabarlar aylanma buferga
 * yoziladi. Har bir qator oldida vaqt belgisi turadi:
 *
 *     [    0.120] paging: yoqildi, kernel kodi faqat o'qish uchun
 *     [   12.450] PID 4 (hack): Segmentation fault - Page Fault
 *
 * Jurnalni 'dmesg' buyrug'i yoki 'cat /proc/kmsg' ko'rsatadi. Bufer to'lsa,
 * eng eski xabarlar ustiga yoziladi (aylanma bufer).
 */
#include "klog.h"
#include "kprintf.h"
#include "drivers.h"
#include "irq.h"
#include "config.h"

#define KLOG_SIZE 16384

static char     ring[KLOG_SIZE];
static uint32_t ring_head = 0;       /* keyingi yoziladigan joy */
static uint32_t ring_len  = 0;       /* saqlangan baytlar (KLOG_SIZE dan oshmaydi) */

static void ring_put(char c)
{
    ring[ring_head] = c;
    ring_head = (ring_head + 1) % KLOG_SIZE;
    if (ring_len < KLOG_SIZE)
        ring_len++;
}

static void klog_write(bool to_serial, const char *fmt, va_list ap)
{
    char line[200];
    uint32_t ms = timer_ticks() * (1000 / TIMER_HZ);
    int n = ksnprintf(line, sizeof(line), "[%5u.%03u] ", ms / 1000, ms % 1000);
    kvsnprintf(line + n, sizeof(line) - n, fmt, ap);

    uint32_t f = irq_save();         /* bir nechta vazifa yozsa ham qatorlar aralashmaydi */
    for (const char *p = line; *p; p++) {
        ring_put(*p);
        if (to_serial)
            serial_putc(*p);
    }
    ring_put('\n');
    if (to_serial)
        serial_putc('\n');
    irq_restore(f);
}

void klog(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    klog_write(true, fmt, ap);
    va_end(ap);
}

void klog_quiet(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    klog_write(false, fmt, ap);
    va_end(ap);
}

uint32_t klog_size(void)
{
    return ring_len;
}

/* Jurnalning offset-baytidan boshlab len baytgacha nusxalash (eng eskisidan boshlab) */
uint32_t klog_read(uint32_t offset, char *buf, uint32_t len)
{
    uint32_t f = irq_save();
    uint32_t n = 0;
    if (offset < ring_len) {
        uint32_t start = (ring_head + KLOG_SIZE - ring_len) % KLOG_SIZE;
        n = ring_len - offset;
        if (n > len)
            n = len;
        for (uint32_t i = 0; i < n; i++)
            buf[i] = ring[(start + offset + i) % KLOG_SIZE];
    }
    irq_restore(f);
    return n;
}
