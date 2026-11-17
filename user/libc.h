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

/* Grafik oynalar (v0.7) — faqat grafik rejimda ('gui' buyrug'idan keyin) ishlaydi */
int      win_create(const char *title, int w, int h);       /* oyna raqami, manfiy — xato */
int      win_update(int id, const unsigned *pixels);        /* eni*bo'yi ta piksel (0x00RRGGBB) */
int      win_event(int id, struct gui_event *ev);           /* 1 — hodisa bor, 0 — yo'q, manfiy — oyna yo'q */
int      win_close(int id);

/* Jarayonlar va kanallar (v0.8) */
int      spawn(const char *path, const char *args, const int io[2]); /* bola PID; io: {stdin, stdout}, -1 — meros, 0 — hammasi meros */
int      wait(int pid, int *code);                                   /* bola tugashini kutish; pid 0 — istalgani. Bola PID */
int      pipe(int fds[2]);                                           /* fds[0] — o'qish, fds[1] — yozish */
int      isatty(int fd);                                             /* 1 — klaviatura/ekran, 0 — kanal */

/* Unix jarayon modeli (v0.10) */
int      fork(void);                              /* otada — bola PID, bolada — 0, xato — manfiy */
int      exec(const char *path, const char *args);/* dasturni almashtirish; faqat xatoda qaytadi */
int      dup(int fd);                             /* fd nusxasi (eng kichik bo'sh raqam) */
int      dup2(int fd, int to);                    /* "to" ni fd ning nusxasiga aylantirish */
int      chdir(const char *path);                 /* shu jarayonning joriy papkasi */
int      getcwd(char *buf, int size);             /* joriy papka; uzunlik yoki xato */

/* Maxsus tugmalar (GUI_EV_KEY dagi key) */
#define KEY_UP     0x11
#define KEY_DOWN   0x12
#define KEY_LEFT   0x17
#define KEY_RIGHT  0x18
#define KEY_ESC    0x1B

/* Satrlar va xotira */
size_t   strlen(const char *s);
int      strcmp(const char *a, const char *b);
char    *strcpy(char *dst, const char *src);
void    *memset(void *dst, int c, size_t n);
void    *memcpy(void *dst, const void *src, size_t n);
int      atoi(const char *s);
int      strncmp(const char *a, const char *b, size_t n);
char    *strstr(const char *hay, const char *needle);

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
