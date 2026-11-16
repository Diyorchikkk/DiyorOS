/*
 * wc — qatorlar, so'zlar va baytlarni sanash (v0.8, Unix dagi wc).
 *
 *   wc fayl          fayldagi qator / so'z / bayt soni
 *   ls | wc          boshqa buyruqning chiqishini sanash (kanal orqali)
 *   wc               klaviaturadan: yozib bo'lgach bo'sh qatorda Ctrl+D
 *
 * Dastur qayerdan o'qiyotganini bilmaydi ham: kanal, fayl yoki klaviatura —
 * hammasi bir xil read() bilan. Bu Unix falsafasi: "hamma narsa — fayl".
 * read() 0 qaytarsa — ma'lumot tugadi (EOF): kanalning yozuvchisi yopildi.
 */
#include "libc.h"

int main(int argc, char **argv)
{
    int fd = 0;
    if (argc > 1) {
        fd = open(argv[1], O_READ);
        if (fd < 0) {
            printf("wc: %s: ochib bo'lmadi\n", argv[1]);
            return 1;
        }
    } else if (isatty(0)) {
        printf("(matn kiriting; tugatish: bo'sh qatorda Ctrl+D)\n");
    }

    static char buf[1024];
    unsigned lines = 0, words = 0, bytes = 0;
    int in_word = 0, n;
    while ((n = read(fd, buf, sizeof(buf))) > 0) {
        bytes += n;
        for (int i = 0; i < n; i++) {
            char c = buf[i];
            if (c == '\n')
                lines++;
            if (c == ' ' || c == '\n' || c == '\t') {
                in_word = 0;
            } else if (!in_word) {
                in_word = 1;
                words++;
            }
        }
    }
    if (fd > 0)
        close(fd);
    printf("%7u %7u %7u", lines, words, bytes);
    if (argc > 1)
        printf(" %s", argv[1]);
    printf("\n");
    return n < 0 ? 1 : 0;
}
