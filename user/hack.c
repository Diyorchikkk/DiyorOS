/*
 * hack — HIMOYA sinovi. Dastur ataylab taqiqlangan ishlarni qilishga urinadi.
 * Har safar faqat shu dastur to'xtatiladi; kernel va boshqa dasturlar
 * ishlashda davom etadi. Bu — user mode (ring 3) ning asosiy maqsadi.
 *
 *   hack       menyu
 *   hack 2     to'g'ridan-to'g'ri 2-sinov
 */
#include "libc.h"

static void test(int n)
{
    switch (n) {
    case 1:
        printf("NULL ko'rsatkichdan o'qish...\n");
        printf("%d\n", *(volatile int *)0);
        break;
    case 2:
        printf("Kernel xotirasiga (0x00100000) yozish...\n");
        *(volatile int *)0x00100000 = 0xDEAD;
        break;
    case 3:
        printf("Ekran xotirasiga (0xB8000) to'g'ridan-to'g'ri yozish...\n");
        *(volatile short *)0xB8000 = 0x4F41;
        break;
    case 4:
        printf("O'z kodimni o'zgartirish (kod segmenti faqat o'qish uchun)...\n");
        *(volatile unsigned char *)test = 0xC3;
        break;
    case 5:
        printf("'cli' buyrug'i (uzilishlarni o'chirish - faqat kernel uchun)...\n");
        __asm__ volatile ("cli");
        break;
    case 6:
        printf("Klaviatura portidan to'g'ridan-to'g'ri o'qish (in 0x60)...\n");
        {
            unsigned char v;
            __asm__ volatile ("inb $0x60, %0" : "=a"(v));
        }
        break;
    case 7: {
        printf("Kernelni aldash: write() ga kernel manzilini berish...\n");
        int r = write(1, (const void *)0x00100000, 16);
        setcolor(LIGHT_GREEN);
        printf("Kernel rad etdi: write() = %d (EFAULT). Dastur ishlashda davom etadi.\n", r);
        setcolor(LIGHT_GREY);
        return;
    }
    case 8:
        printf("Stekni to'ldirish (cheksiz rekursiya)...\n");
        test(8);
        break;
    default:
        printf("Bunday sinov yo'q\n");
        return;
    }
    printf("Bu qator chiqmasligi kerak edi!\n");
}

int main(int argc, char **argv)
{
    if (argc > 1) {
        test(atoi(argv[1]));
        return 0;
    }

    setcolor(YELLOW);
    printf("=== Himoya sinovi: dastur kernelni buzishga urinadi ===\n");
    setcolor(LIGHT_GREY);
    printf("  1. NULL ko'rsatkich          5. 'cli' buyrug'i\n");
    printf("  2. Kernel xotirasiga yozish   6. Qurilma porti (in)\n");
    printf("  3. Ekran xotirasiga yozish    7. Syscall ga kernel manzili\n");
    printf("  4. O'z kodini o'zgartirish    8. Stekni to'ldirish\n");
    printf("Tanlang (1-8): ");

    char line[8];
    readline(line, sizeof(line));
    test(atoi(line));
    return 0;
}
