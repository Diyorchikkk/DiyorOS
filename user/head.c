/*
 * head — birinchi N qatorni chiqarish (v0.8).
 *
 *   head fayl            birinchi 10 qator
 *   head 3 fayl          birinchi 3 qator
 *   sanoq 1000000 | head 5
 *
 * Oxirgi misolda head 5 qatordan keyin TUGAYDI va kanalning o'qish uchi
 * yopiladi. sanoq keyingi write() da EPIPE oladi va kernel uni to'xtatadi
 * (Unix dagi SIGPIPE) — million sonni bekorga hisoblab o'tirmaydi.
 */
#include "libc.h"

int main(int argc, char **argv)
{
    int count = 10, a = 1;
    if (a < argc && argv[a][0] >= '0' && argv[a][0] <= '9')
        count = atoi(argv[a++]);
    int fd = 0;
    if (a < argc) {
        fd = open(argv[a], O_READ);
        if (fd < 0) {
            printf("head: %s: ochib bo'lmadi\n", argv[a]);
            return 1;
        }
    }
    char buf[256];
    int lines = 0, n;
    while (lines < count && (n = read(fd, buf, sizeof(buf))) > 0) {
        int end = 0;
        while (end < n && lines < count)
            if (buf[end++] == '\n')
                lines++;
        write(1, buf, end);
    }
    if (fd > 0)
        close(fd);
    return 0;
}
