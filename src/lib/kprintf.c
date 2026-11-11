/*
 * kprintf.c — printf ning soddalashtirilgan kernel varianti.
 * stdarg.h kompilyatorning o'zida bor (libc emas), shuning uchun
 * o'zgaruvchan argumentlardan foydalana olamiz.
 */
#include <stdarg.h>
#include <stdint.h>
#include "kprintf.h"
#include "vga.h"

/* Sonni berilgan asosda (10 yoki 16) satrga aylantirib chiqarish */
static void print_unsigned(uint32_t value, uint32_t base, int width, char pad)
{
    char buf[16];
    int i = 0;
    const char *digits = "0123456789ABCDEF";

    do {
        buf[i++] = digits[value % base];
        value /= base;
    } while (value);

    while (i < width) {         /* kenglikka yetguncha to'ldirish */
        vga_putc(pad);
        width--;
    }
    while (i--)
        vga_putc(buf[i]);
}

static void print_signed(int32_t value, int width, char pad)
{
    if (value < 0) {
        vga_putc('-');
        print_unsigned((uint32_t)(-(value + 1)) + 1u, 10, width > 0 ? width - 1 : 0, pad);
    } else {
        print_unsigned((uint32_t)value, 10, width, pad);
    }
}

void kprintf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);

    for (; *fmt; fmt++) {
        if (*fmt != '%') {
            vga_putc(*fmt);
            continue;
        }
        fmt++;

        char pad = ' ';
        int width = 0;
        int left = 0;               /* '-' — chapga tekislash (faqat %s uchun) */
        if (*fmt == '-') {
            left = 1;
            fmt++;
        }
        if (*fmt == '0') {
            pad = '0';
            fmt++;
        }
        while (*fmt >= '0' && *fmt <= '9')
            width = width * 10 + (*fmt++ - '0');

        switch (*fmt) {
        case 'd': print_signed(va_arg(ap, int32_t), width, pad); break;
        case 'u': print_unsigned(va_arg(ap, uint32_t), 10, width, pad); break;
        case 'x': print_unsigned(va_arg(ap, uint32_t), 16, width, pad); break;
        case 'c': vga_putc((char)va_arg(ap, int)); break;
        case 's': {
            const char *s = va_arg(ap, const char *);
            if (!s)
                s = "(null)";
            int len = 0;
            while (s[len])
                len++;
            if (!left)
                for (int i = len; i < width; i++)
                    vga_putc(' ');
            vga_puts(s);
            if (left)
                for (int i = len; i < width; i++)
                    vga_putc(' ');
            break;
        }
        case '%': vga_putc('%'); break;
        case '\0': fmt--; break;
        default: vga_putc('%'); vga_putc(*fmt); break;
        }
    }

    va_end(ap);
}
