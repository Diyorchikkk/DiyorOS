/*
 * sanoq — sonlarni ketma-ket chiqarish (Unix dagi seq), kanallarni sinash uchun.
 *
 *   sanoq 10             1 dan 10 gacha
 *   sanoq 5 15           5 dan 15 gacha
 *   sanoq 100000 | wc    katta oqim: 4 KB li kanal buferi ko'p marta to'lib-bo'shaydi
 *   sanoq 1000 | grep 7 | wc
 */
#include "libc.h"

int main(int argc, char **argv)
{
    int from = 1, to = 10;
    if (argc == 2)
        to = atoi(argv[1]);
    else if (argc >= 3) {
        from = atoi(argv[1]);
        to = atoi(argv[2]);
    }
    char buf[512];
    int len = 0;
    for (int v = from; v <= to; v++) {
        /* printf har chaqiruvda syscall qiladi — o'zimiz buferlaymiz (tezroq) */
        char tmp[12];
        int n = 0;
        unsigned u = v < 0 ? (unsigned)-v : (unsigned)v;
        do {
            tmp[n++] = '0' + u % 10;
            u /= 10;
        } while (u);
        if (v < 0)
            tmp[n++] = '-';
        if (len + n + 1 > (int)sizeof(buf)) {
            if (write(1, buf, len) < 0)
                return 1;                     /* kanal yopilgan */
            len = 0;
        }
        while (n)
            buf[len++] = tmp[--n];
        buf[len++] = '\n';
    }
    if (len)
        write(1, buf, len);
    return 0;
}
