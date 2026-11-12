/*
 * libc.c — DiyorOS dasturlari uchun kichik standart kutubxona.
 */
#include <stdarg.h>
#include "libc.h"

/* ------------------------------ Syscall lar ------------------------------ */

/* int 0x80: EAX = raqam, EBX/ECX/EDX = argumentlar, natija — EAX */
static inline int syscall3(int n, int a, int b, int c)
{
    int ret;
    __asm__ volatile ("int $0x80" : "=a"(ret) : "a"(n), "b"(a), "c"(b), "d"(c) : "memory");
    return ret;
}

void exit(int code)
{
    syscall3(SYS_EXIT, code, 0, 0);
    for (;;)
        ;
}

int      write(int fd, const void *buf, unsigned len) { return syscall3(SYS_WRITE, fd, (int)buf, len); }
int      read(int fd, void *buf, unsigned len)        { return syscall3(SYS_READ, fd, (int)buf, len); }
int      open(const char *path, int flags)            { return syscall3(SYS_OPEN, (int)path, flags, 0); }
int      close(int fd)                                { return syscall3(SYS_CLOSE, fd, 0, 0); }
void     sleep(unsigned ms)                           { syscall3(SYS_SLEEP, ms, 0, 0); }
int      getpid(void)                                 { return syscall3(SYS_GETPID, 0, 0, 0); }
unsigned uptime(void)                                 { return syscall3(SYS_UPTIME, 0, 0, 0); }
void     yield(void)                                  { syscall3(SYS_YIELD, 0, 0, 0); }
void    *sbrk(int increment)                          { return (void *)syscall3(SYS_SBRK, increment, 0, 0); }
int      sysinfo(struct sysinfo *info)                { return syscall3(SYS_SYSINFO, (int)info, 0, 0); }
void     setcolor(int color)                          { syscall3(SYS_SETCOLOR, color, 0, 0); }
void     clear(void)                                  { syscall3(SYS_CLEAR, 0, 0, 0); }

/* ---------------------------- Satrlar, xotira ---------------------------- */

size_t strlen(const char *s)
{
    size_t n = 0;
    while (s[n])
        n++;
    return n;
}

int strcmp(const char *a, const char *b)
{
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return (unsigned char)*a - (unsigned char)*b;
}

char *strcpy(char *dst, const char *src)
{
    char *d = dst;
    while ((*d++ = *src++))
        ;
    return dst;
}

void *memset(void *dst, int c, size_t n)
{
    unsigned char *d = dst;
    while (n--)
        *d++ = (unsigned char)c;
    return dst;
}

void *memcpy(void *dst, const void *src, size_t n)
{
    unsigned char *d = dst;
    const unsigned char *s = src;
    while (n--)
        *d++ = *s++;
    return dst;
}

int atoi(const char *s)
{
    int v = 0, neg = 0;
    while (*s == ' ')
        s++;
    if (*s == '-') {
        neg = 1;
        s++;
    }
    while (*s >= '0' && *s <= '9')
        v = v * 10 + (*s++ - '0');
    return neg ? -v : v;
}

/* -------------------------------- printf --------------------------------- */

typedef struct {
    char buf[256];
    int  len;
    int  total;
} out_t;

static void flush(out_t *o)
{
    if (o->len)
        write(1, o->buf, o->len);
    o->len = 0;
}

static void emit(out_t *o, char c)
{
    if (o->len == (int)sizeof(o->buf))
        flush(o);
    o->buf[o->len++] = c;
    o->total++;
}

static void emit_uint(out_t *o, unsigned v, unsigned base, int width, char pad)
{
    char tmp[12];
    int n = 0;
    do {
        tmp[n++] = "0123456789abcdef"[v % base];
        v /= base;
    } while (v);
    while (n < width--)
        emit(o, pad);
    while (n)
        emit(o, tmp[--n]);
}

int printf(const char *fmt, ...)
{
    out_t o;
    o.len = o.total = 0;
    va_list ap;
    va_start(ap, fmt);

    for (; *fmt; fmt++) {
        if (*fmt != '%') {
            emit(&o, *fmt);
            continue;
        }
        fmt++;
        char pad = ' ';
        int width = 0, left = 0;
        if (*fmt == '-') { left = 1; fmt++; }
        if (*fmt == '0') { pad = '0'; fmt++; }
        while (*fmt >= '0' && *fmt <= '9')
            width = width * 10 + (*fmt++ - '0');

        switch (*fmt) {
        case 'd': {
            int v = va_arg(ap, int);
            if (v < 0) {
                emit(&o, '-');
                emit_uint(&o, (unsigned)(-(v + 1)) + 1u, 10, width ? width - 1 : 0, pad);
            } else {
                emit_uint(&o, (unsigned)v, 10, width, pad);
            }
            break;
        }
        case 'u': emit_uint(&o, va_arg(ap, unsigned), 10, width, pad); break;
        case 'x': emit_uint(&o, va_arg(ap, unsigned), 16, width, pad); break;
        case 'p': emit(&o, '0'); emit(&o, 'x'); emit_uint(&o, va_arg(ap, unsigned), 16, 8, '0'); break;
        case 'c': emit(&o, (char)va_arg(ap, int)); break;
        case 's': {
            const char *s = va_arg(ap, const char *);
            if (!s)
                s = "(null)";
            int len = strlen(s);
            if (!left)
                for (int i = len; i < width; i++)
                    emit(&o, ' ');
            while (*s)
                emit(&o, *s++);
            if (left)
                for (int i = len; i < width; i++)
                    emit(&o, ' ');
            break;
        }
        case '%': emit(&o, '%'); break;
        case 0: fmt--; break;
        default: emit(&o, '%'); emit(&o, *fmt); break;
        }
    }
    va_end(ap);
    flush(&o);
    return o.total;
}

int putchar(int c)
{
    char ch = (char)c;
    write(1, &ch, 1);
    return c;
}

int puts(const char *s)
{
    write(1, s, strlen(s));
    write(1, "\n", 1);
    return 0;
}

int readline(char *buf, int max)
{
    int n = read(0, buf, max - 1);
    if (n < 0)
        n = 0;
    if (n > 0 && buf[n - 1] == '\n')
        n--;
    buf[n] = 0;
    return n;
}

/* --------------------------------- malloc --------------------------------- */
/*
 * Oddiy "First Fit" ajratuvchi. Kernel dagi kmalloc ga o'xshash, lekin
 * xotirani sbrk() syscall i orqali kernel dan so'raydi: kernel dastur
 * hududiga yangi sahifalar qo'shib beradi.
 */
typedef struct block {
    size_t        size;
    int           free;
    struct block *next;
} block_t;

static block_t *heap_head = 0;

void *malloc(size_t size)
{
    if (size == 0)
        return 0;
    size = (size + 7) & ~7u;

    block_t *last = 0;
    for (block_t *b = heap_head; b; last = b, b = b->next)
        if (b->free && b->size >= size) {
            if (b->size >= size + sizeof(block_t) + 16) {   /* katta blokni bo'lamiz */
                block_t *rest = (block_t *)((char *)(b + 1) + size);
                rest->size = b->size - size - sizeof(block_t);
                rest->free = 1;
                rest->next = b->next;
                b->next = rest;
                b->size = size;
            }
            b->free = 0;
            return b + 1;
        }

    block_t *b = sbrk(sizeof(block_t) + size);
    if (b == (void *)-1)
        return 0;
    b->size = size;
    b->free = 0;
    b->next = 0;
    if (last)
        last->next = b;
    else
        heap_head = b;
    return b + 1;
}

void free(void *ptr)
{
    if (!ptr)
        return;
    block_t *b = (block_t *)ptr - 1;
    b->free = 1;
    /* qo'shni bo'sh bloklarni birlashtirish */
    for (block_t *x = heap_head; x; x = x->next)
        while (x->free && x->next && x->next->free &&
               (char *)(x + 1) + x->size == (char *)x->next) {
            x->size += sizeof(block_t) + x->next->size;
            x->next = x->next->next;
        }
}

/* ------------------------------- Tasodifiy -------------------------------- */

static unsigned rng = 1;

void srand(unsigned seed) { rng = seed ? seed : 1; }

unsigned rand(void)
{
    rng = rng * 1103515245u + 12345u;
    return (rng >> 16) & 0x7FFF;
}
