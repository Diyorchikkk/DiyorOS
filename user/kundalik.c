/*
 * kundalik — fayl syscall lari: open, write, read, close.
 * Kiritilgan qatorni /disk/kundalik.txt oxiriga qo'shadi va butun faylni ko'rsatadi.
 */
#include "libc.h"

/* v0.6: disk ulangan bo'lsa — diskka (o'chirib-yoqqanda ham saqlanadi) */
static const char *FILE_PATH = "/disk/kundalik.txt";

int main(void)
{
    char line[200];
    printf("Bugun nima qildingiz? ");
    int n = readline(line, sizeof(line));
    if (n == 0) {
        printf("Bo'sh qator - hech narsa yozilmadi.\n");
        return 1;
    }

    int fd = open(FILE_PATH, O_WRITE | O_CREATE | O_APPEND);
    if (fd < 0) {                          /* disk yo'q — xotiradagi uy papkasiga */
        FILE_PATH = "/home/diyorbek/kundalik.txt";
        fd = open(FILE_PATH, O_WRITE | O_CREATE | O_APPEND);
    }
    if (fd < 0) {
        printf("Faylni ochib bo'lmadi (xato %d)\n", fd);
        return 1;
    }
    unsigned s = uptime() / 1000;
    char stamp[32];
    int len = 0;
    stamp[len++] = '[';
    /* oddiy itoa */
    char digits[12];
    int d = 0;
    do {
        digits[d++] = '0' + s % 10;
        s /= 10;
    } while (s);
    while (d)
        stamp[len++] = digits[--d];
    memcpy(stamp + len, "-soniya] ", 9);
    len += 9;
    write(fd, stamp, len);
    write(fd, line, n);
    write(fd, "\n", 1);
    close(fd);
    printf("Yozildi (fd %d orqali).\n", fd);

    setcolor(LIGHT_CYAN);
    printf("--- %s ---\n", FILE_PATH);
    setcolor(LIGHT_GREY);
    fd = open(FILE_PATH, O_READ);
    char buf[256];
    int r;
    while ((r = read(fd, buf, sizeof(buf))) > 0)
        write(1, buf, r);
    close(fd);
    return 0;
}
