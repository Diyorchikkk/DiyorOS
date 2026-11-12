/*
 * isolate — xotira izolyatsiyasi namoyishi.
 *
 * Ikki nusxasini fonda ishga tushiring:
 *     isolate &
 *     isolate &
 * Ikkalasi ham 'counter' o'zgaruvchisini BIR XIL virtual manzilda ko'radi,
 * lekin qiymatlari har xil: har bir jarayonning o'z Page Directory si bor,
 * bir xil virtual manzil har xil fizik frame larga tushadi.
 */
#include "libc.h"

int counter = 0;            /* global o'zgaruvchi — .data/.bss da */

int main(void)
{
    int pid = getpid();
    counter = pid * 1000;

    for (int i = 0; i < 10; i++) {
        setcolor(pid % 2 ? LIGHT_CYAN : YELLOW);
        printf("[PID %d] &counter = %p, counter = %d\n", pid, (unsigned)&counter, counter);
        setcolor(LIGHT_GREY);
        counter++;
        sleep(1000);
    }
    return 0;
}
