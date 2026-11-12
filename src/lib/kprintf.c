/*
 * kprintf.c — printf ning soddalashtirilgan kernel varianti.
 * stdarg.h kompilyatorning o'zida bor (libc emas), shuning uchun
 * o'zgaruvchan argumentlardan foydalana olamiz.
 *
 * Formatlash bitta joyda (vformat) yozilgan, natija esa "chiqarish
 * funksiyasi" orqali yuboriladi:
 *   kprintf   -> ekranga (vga_putc)
 *   ksnprintf -> xotiradagi buferga (v0.4: fayllar va /proc uchun)
 */
#include <stdarg.h>
#include <stdint.h>
#include "kprintf.h"
#include "vga.h"

typedef void (*out_fn)(char c, void *ctx);

static void print_unsigned(out_fn out, void *ctx, uint32_t value, uint32_t base, int width, char pad)
{
    char buf[16];
    int i = 0;
    const char *digits = "0123456789ABCDEF";

    do {
        buf[i++] = digits[value % base];
        value /= base;
    } while (value);

    while (i < width) {         /* kenglikka yetguncha to'ldirish */
        out(pad, ctx);
        width--;
    }
    while (i--)
        out(buf[i], ctx);
}

static void print_signed(out_fn out, void *ctx, int32_t value, int width, char pad)
{
    if (value < 0) {
        out('-', ctx);
        print_unsigned(out, ctx, (uint32_t)(-(value + 1)) + 1u, 10, width > 0 ? width - 1 : 0, pad);
    } else {
        print_unsigned(out, ctx, (uint32_t)value, 10, width, pad);
    }
}

static void vformat(out_fn out, void *ctx, const char *fmt, va_list ap)
{
    for (; *fmt; fmt++) {
        if (*fmt != '%') {
            out(*fmt, ctx);
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
        case 'd': print_signed(out, ctx, va_arg(ap, int32_t), width, pad); break;
        case 'u': print_unsigned(out, ctx, va_arg(ap, uint32_t), 10, width, pad); break;
        case 'x': print_unsigned(out, ctx, va_arg(ap, uint32_t), 16, width, pad); break;
        case 'c': out((char)va_arg(ap, int), ctx); break;
        case 's': {
            const char *s = va_arg(ap, const char *);
            if (!s)
                s = "(null)";
            int len = 0;
            while (s[len])
                len++;
            if (!left)
                for (int i = len; i < width; i++)
                    out(' ', ctx);
            while (*s)
                out(*s++, ctx);
            if (left)
                for (int i = len; i < width; i++)
                    out(' ', ctx);
            break;
        }
        case '%': out('%', ctx); break;
        case '\0': fmt--; break;
        default: out('%', ctx); out(*fmt, ctx); break;
        }
    }
}

/* ------------------------------- Ekranga -------------------------------- */

static void out_screen(char c, void *ctx)
{
    (void)ctx;
    vga_putc(c);
}

void kprintf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vformat(out_screen, 0, fmt, ap);
    va_end(ap);
}

/* ------------------------------- Buferga -------------------------------- */

typedef struct {
    char    *buf;
    uint32_t size;
    uint32_t len;      /* sig'masa ham hisoblanadi — kerakli hajmni bilish uchun */
} buf_ctx_t;

static void out_buffer(char c, void *ctx)
{
    buf_ctx_t *b = ctx;
    if (b->len + 1 < b->size)
        b->buf[b->len] = c;
    b->len++;
}

int kvsnprintf(char *buf, uint32_t size, const char *fmt, va_list ap)
{
    buf_ctx_t b = { buf, size, 0 };
    vformat(out_buffer, &b, fmt, ap);
    if (size)
        buf[b.len < size ? b.len : size - 1] = 0;
    return (int)b.len;
}

int ksnprintf(char *buf, uint32_t size, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = kvsnprintf(buf, size, fmt, ap);
    va_end(ap);
    return n;
}
