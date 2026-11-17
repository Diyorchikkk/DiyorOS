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
#define SYS_WRITE     1   /* write(fd, buf, uzunlik)         — odatda 1: ekran, 2: ekran (xato), 3+: fayl */
#define SYS_READ      2   /* read(fd, buf, uzunlik)          — odatda 0: klaviatura (qator), 3+: fayl */
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
/* v0.7: grafik oynalar (faqat grafik rejimda; aks holda -19 qaytaradi) */
#define SYS_WIN_CREATE 13 /* win_create(nom, eni, bo'yi)     — oyna raqami */
#define SYS_WIN_UPDATE 14 /* win_update(raqam, piksellar)    — eni*bo'yi ta 0x00RRGGBB ni ekranga */
#define SYS_WIN_EVENT  15 /* win_event(raqam, struct gui_event *) — 1: hodisa bor, 0: yo'q */
#define SYS_WIN_CLOSE  16 /* win_close(raqam) */
/* v0.8: jarayonlar va kanallar */
#define SYS_SPAWN    17   /* spawn(yo'l, argumentlar, int io[2]) — bola PID; io: {stdin, stdout}, -1 — meros */
#define SYS_WAIT     18   /* wait(pid, int *kod)             — bola tugashini kutish; pid 0 — istalgani. Bola PID */
#define SYS_PIPE     19   /* pipe(int fds[2])                — fds[0] o'qish, fds[1] yozish uchi */
/* v0.10: Unix jarayon modeli */
#define SYS_FORK     20   /* fork()                          — otada bola PID, bolada 0 */
#define SYS_EXEC     21   /* exec(yo'l, argumentlar)         — dasturni almashtirish (faqat xatoda qaytadi) */
#define SYS_DUP      22   /* dup(fd)                         — nusxa: eng kichik bo'sh raqam */
#define SYS_DUP2     23   /* dup2(fd, yangi)                 — "yangi" ni yopib, fd nusxasini qo'yish */
#define SYS_CHDIR    24   /* chdir(yo'l)                     — jarayonning joriy papkasi */
#define SYS_GETCWD   25   /* getcwd(buf, hajm)               — joriy papka yo'li */
#define SYS_COUNT    26

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
    unsigned int stdio;          /* v0.8: SI_STDIN_PIPE | SI_STDOUT_PIPE — yo'naltirilganmi */
};
#define SI_STDIN_PIPE  1         /* stdin klaviatura emas (kanal) */
#define SI_STDOUT_PIPE 2         /* stdout ekran emas (kanal) */

/* Oyna hodisalari (v0.7) */
#define GUI_EV_NONE       0
#define GUI_EV_KEY        1   /* key — belgi yoki maxsus kod (strelkalar: 0x11-0x18) */
#define GUI_EV_MOUSE_DOWN 2   /* x, y — oyna ichidagi koordinata; buttons: 1 chap, 2 o'ng */
#define GUI_EV_MOUSE_UP   3
#define GUI_EV_MOUSE_MOVE 4
#define GUI_EV_WHEEL      5   /* key — g'ildirak: -1 yuqoriga, +1 pastga */
#define GUI_EV_CLOSE      6   /* foydalanuvchi oynaning [x] tugmasini bosdi */

struct gui_event {
    int type;
    int x, y;
    int key;
    int buttons;
};

#endif
