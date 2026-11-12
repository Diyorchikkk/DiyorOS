/*
 * libc.h — DiyorOS dasturlari uchun kichik standart kutubxona.
 *
 * Linux dagi glibc o'rniga. Har bir funksiya oxir-oqibat "int 0x80"
 * (syscall) orqali kernelga murojaat qiladi — dastur o'zi ekranga ham,
 * faylga ham to'g'ridan-to'g'ri tega olmaydi.
 */
#ifndef LIBC_H
#define LIBC_H

#include <stddef.h>
#include <stdint.h>
#include "syscall_nums.h"

/* Ranglar (setcolor uchun) */
enum {
    BLACK = 0, BLUE, GREEN, CYAN, RED, MAGENTA, BROWN, LIGHT_GREY,
    DARK_GREY, LIGHT_BLUE, LIGHT_GREEN, LIGHT_CYAN, LIGHT_RED, LIGHT_MAGENTA, YELLOW, WHITE
};

/* Tizim chaqiruvlari */
void     exit(int code) __attribute__((noreturn));
int      write(int fd, const void *buf, unsigned len);
int      read(int fd, void *buf, unsigned len);
int      open(const char *path, int flags);
int      close(int fd);
void     sleep(unsigned ms);
int      getpid(void);
unsigned uptime(void);                   /* millisekund */
void     yield(void);
void    *sbrk(int increment);
int      sysinfo(struct sysinfo *info);
void     setcolor(int color);
void     clear(void);

/* Satrlar va xotira */
size_t   strlen(const char *s);
int      strcmp(const char *a, const char *b);
char    *strcpy(char *dst, const char *src);
void    *memset(void *dst, int c, size_t n);
void    *memcpy(void *dst, const void *src, size_t n);
int      atoi(const char *s);

/* Kiritish / chiqarish */
int      printf(const char *fmt, ...);
int      puts(const char *s);
int      putchar(int c);
int      readline(char *buf, int max);   /* klaviaturadan qator ('\n' siz) */

/* Dinamik xotira (sbrk ustida) */
void    *malloc(size_t size);
void     free(void *ptr);

/* Tasodifiy son */
void     srand(unsigned seed);
unsigned rand(void);

#endif
