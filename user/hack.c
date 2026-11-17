/*
 * hack — HIMOYA sinovi. Dastur ataylab taqiqlangan ishlarni qilishga urinadi.
 * Har safar faqat shu dastur to'xtatiladi; kernel va boshqa dasturlar
 * ishlashda davom etadi. Bu — user mode (ring 3) ning asosiy maqsadi.
 *
 *   hack       menyu
 *   hack 2     to'g'ridan-to'g'ri 2-sinov
 *   hack 9     "fork bomb" (v0.10): jarayonlar to'xtovsiz ko'payadi. Kernel chegaralari
 *              (bittada 4 bola, tizimda 16 vazifa) uni to'xtatadi; Ctrl+C butun guruhni o'ldiradi
 */
#include "libc.h"

/* Har chaqiruvda stekdan ~1 KB oladi; 64 KB lik stek tugaganda Page Fault bo'ladi.
   Cheksiz rekursiya bu yerda ataylab — kompilyator ogohlantirishini o'chiramiz */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Winfinite-recursion"
static int overflow(int depth)
{
    volatile char pad[1024];
    pad[0] = (char)depth;
    return overflow(depth + 1) + pad[0];
}
#pragma GCC diagnostic pop

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
        overflow(1);
        break;
    case 9: {
        printf("Fork bomb: har bir jarayon to'xtovsiz fork() qiladi...\n");
        int leader = getpid(), err = 0;
        for (;;) {
            int pid = fork();
            if (pid == 0)
                continue;                   /* bola ham ko'paytirishni davom ettiradi */
            if (pid < 0) {
                err = pid;
                break;
            }
        }
        if (getpid() == leader) {
            sleep(300);                     /* qolganlari ham chegaraga yetib olsin */
            struct sysinfo si;
            sysinfo(&si);
            setcolor(LIGHT_GREEN);
            printf("Kernel to'xtatdi: fork() = %d. Tizimda %u ta vazifa (maksimum 16),\n", err, si.task_count);
            printf("har bir jarayonda ko'pi bilan 4 bola. Kernel va shell ishlayapti.\n");
            setcolor(LIGHT_GREY);
            printf("Hamma jarayonlar bitta guruhda: Ctrl+C ularning barchasini to'xtatadi.\n");
        }
        for (;;)
            sleep(1000);
    }
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
    printf("                                9. Fork bomb (ko'payish)\n");
    printf("Tanlang (1-9): ");

    char line[8];
    readline(line, sizeof(line));
    test(atoi(line));
    return 0;
}
