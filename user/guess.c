/*
 * guess — son topish o'yini. Klaviaturadan kiritish (read syscall) ni ko'rsatadi.
 */
#include "libc.h"

int main(void)
{
    srand(uptime() * 7919 + getpid());
    int secret = rand() % 100 + 1;
    int tries = 0;
    char line[32];

    setcolor(YELLOW);
    printf("=== Son topish o'yini ===\n");
    setcolor(LIGHT_GREY);
    printf("Men 1 dan 100 gacha son o'yladim. Topa olasizmi? (chiqish: q)\n");

    for (;;) {
        printf("Taxminingiz: ");
        readline(line, sizeof(line));
        if (line[0] == 'q')
            break;
        int n = atoi(line);
        if (n < 1 || n > 100) {
            printf("1 dan 100 gacha son kiriting.\n");
            continue;
        }
        tries++;
        if (n < secret) {
            printf("Kattaroq son!\n");
        } else if (n > secret) {
            printf("Kichikroq son!\n");
        } else {
            setcolor(LIGHT_GREEN);
            printf("Topdingiz! Son %d edi. Urinishlar: %d\n", secret, tries);
            setcolor(LIGHT_GREY);
            return 0;
        }
    }
    printf("O'yladigan sonim %d edi.\n", secret);
    return 1;
}
