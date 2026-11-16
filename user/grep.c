/*
 * grep — naqsh uchragan qatorlarni chiqarish (v0.8).
 *
 *   grep naqsh fayl        faylda qidirish
 *   ls /bin | grep o       boshqa buyruq chiqishidan qidirish
 *   grep -v naqsh ...      naqsh UCHRAMAGAN qatorlar
 *   grep -c naqsh ...      faqat mos qatorlar soni
 *   grep -i naqsh ...      katta-kichik harf farqsiz
 *
 * Chiqish ekranga ketsa — topilgan joy rangli; kanalga ketsa — oddiy matn
 * (keyingi dastur rang kodlarini emas, toza matnni olishi kerak).
 */
#include "libc.h"

static int opt_v = 0, opt_c = 0, opt_i = 0;

static char lower(char c)
{
    return (c >= 'A' && c <= 'Z') ? c + 32 : c;
}

/* line ichida pat qayerda uchraydi (-1 — yo'q) */
static int find(const char *line, const char *pat)
{
    int pn = strlen(pat);
    for (int i = 0; line[i]; i++) {
        int j = 0;
        while (j < pn && line[i + j] &&
               (opt_i ? lower(line[i + j]) == lower(pat[j]) : line[i + j] == pat[j]))
            j++;
        if (j == pn)
            return i;
    }
    return pn ? -1 : 0;
}

static unsigned matches = 0;
static int color = 0;

static void handle(const char *line, const char *pat)
{
    int pos = find(line, pat);
    if ((pos >= 0) == !opt_v) {
        matches++;
        if (opt_c)
            return;
        if (color && pos >= 0 && !opt_v) {
            write(1, line, pos);
            setcolor(LIGHT_RED);
            write(1, line + pos, strlen(pat));
            setcolor(LIGHT_GREY);
            printf("%s\n", line + pos + strlen(pat));
        } else {
            printf("%s\n", line);
        }
    }
}

int main(int argc, char **argv)
{
    int i = 1;
    for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++)
        for (const char *o = argv[i] + 1; *o; o++) {
            if (*o == 'v') opt_v = 1;
            else if (*o == 'c') opt_c = 1;
            else if (*o == 'i') opt_i = 1;
            else {
                printf("grep: noma'lum kalit -%c\n", *o);
                return 2;
            }
        }
    if (i >= argc) {
        printf("Ishlatish: grep [-v] [-c] [-i] naqsh [fayl]\n");
        return 2;
    }
    const char *pat = argv[i++];
    int fd = 0;
    if (i < argc) {
        fd = open(argv[i], O_READ);
        if (fd < 0) {
            printf("grep: %s: ochib bo'lmadi\n", argv[i]);
            return 2;
        }
    }
    color = isatty(1);

    static char buf[512];
    static char line[256];
    int len = 0, n;
    while ((n = read(fd, buf, sizeof(buf))) > 0) {
        for (int k = 0; k < n; k++) {
            char c = buf[k];
            if (c == '\n' || len == (int)sizeof(line) - 1) {
                line[len] = 0;
                handle(line, pat);
                len = 0;
                if (c == '\n')
                    continue;
            }
            line[len++] = c;
        }
    }
    if (len) {
        line[len] = 0;
        handle(line, pat);
    }
    if (fd > 0)
        close(fd);
    if (opt_c)
        printf("%u\n", matches);
    return matches ? 0 : 1;                   /* Unix: 0 — topildi, 1 — topilmadi */
}
