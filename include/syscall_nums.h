/*
 * syscall_nums.h — tizim chaqiruvlari (syscall) raqamlari.
 * Bu fayl ham kernel, ham dasturlar (user/libc) tomonidan ishlatiladi:
 * ikkalasi bir xil raqamlarni bilishi shart.
 *
 * Chaqiruv tartibi (int 0x80):
 *   EAX = syscall raqami, EBX/ECX/EDX = 1/2/3-argumentlar
 *   natija EAX da qaytadi (manfiy — xato kodi)
 */
#ifndef SYSCALL_NUMS_H
#define SYSCALL_NUMS_H

#define SYS_EXIT      0   /* exit(kod)                       — dasturni tugatish */
#define SYS_WRITE     1   /* write(fd, buf, uzunlik)         — 1: ekran, 2: ekran (xato), 3+: fayl */
#define SYS_READ      2   /* read(fd, buf, uzunlik)          — 0: klaviatura (qator), 3+: fayl */
#define SYS_OPEN      3   /* open(yo'l, flaglar)             — fd qaytaradi */
#define SYS_CLOSE     4   /* close(fd) */
#define SYS_SLEEP     5   /* sleep(ms) */
#define SYS_GETPID    6   /* getpid() */
#define SYS_UPTIME    7   /* uptime() — millisekund */
#define SYS_YIELD     8   /* yield()                         — navbatni berish */
#define SYS_SBRK      9   /* sbrk(o'zgarish)                 — heap ni kattalashtirish, eski oxirini qaytaradi */
#define SYS_SYSINFO  10   /* sysinfo(struct sysinfo *) */
#define SYS_SETCOLOR 11   /* setcolor(rang)                  — matn rangi (0-15) */
#define SYS_CLEAR    12   /* clear()                         — ekranni tozalash */
#define SYS_COUNT    13

/* open() flaglari (VFS dagi bilan bir xil) */
#define O_READ    0x01
#define O_WRITE   0x02
#define O_CREATE  0x04
#define O_TRUNC   0x08
#define O_APPEND  0x10

struct sysinfo {
    unsigned int mem_total_kb;
    unsigned int mem_free_kb;
    unsigned int uptime_ms;
    unsigned int pid;
    unsigned int task_count;
    unsigned int user_pages;     /* shu jarayon egallagan sahifalar */
    char         os_name[16];
    char         os_version[8];
};

#endif
