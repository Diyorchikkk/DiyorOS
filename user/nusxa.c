/*
 * nusxa — fork(), copy-on-write va exec() namoyishi (v0.10).
 *
 *   nusxa      1) fork: bitta dastur ikki jarayonga bo'linadi; bir xil manzildagi
 *                 o'zgaruvchi har birida o'z qiymatiga ega bo'ladi
 *              2) copy-on-write: 1 MB massiv fork da NUSXALANMAYDI — sahifalar
 *                 faqat bola ularga yozganda birma-bir ko'chiriladi
 *              3) fork + exec + wait: shell dasturni shunday ishga tushiradi;
 *                 exec xato bo'lsa, jarayon eski dasturda davom etadi
 */
#include "libc.h"

#define BIG (1024 * 1024)

int shared = 100;              /* .data — bitta sahifa */
static char big[BIG];          /* .bss — 256 sahifa, yuklanishda ajratiladi */

static unsigned free_kb(void)
{
    struct sysinfo si;
    sysinfo(&si);
    return si.mem_free_kb;
}

static void title(const char *s)
{
    setcolor(YELLOW);
    printf("%s\n", s);
    setcolor(LIGHT_GREY);
}

int main(void)
{
    int code = 0;

    /* ---------------- 1) fork ---------------- */
    title("=== 1) fork(): jarayon ikkiga bo'linadi ===");
    printf("[ota %d] shared = %d, manzili %p\n", getpid(), shared, (unsigned)&shared);
    int pid = fork();
    if (pid < 0) {
        printf("fork xato: %d\n", pid);
        return 1;
    }
    if (pid == 0) {                        /* bu yerga faqat BOLA keladi */
        shared = 200;
        setcolor(LIGHT_CYAN);
        printf("  [bola %d] fork() = 0. shared = 200 qildim (manzili %p)\n", getpid(), (unsigned)&shared);
        setcolor(LIGHT_GREY);
        exit(7);
    }
    wait(pid, &code);                      /* bu yerga faqat OTA keladi */
    printf("[ota %d] fork() = %d. Bola tugadi, kodi %d. Menda shared = %d - o'zgarmadi.\n",
           getpid(), pid, code, shared);
    printf("  Manzil bir xil, fizik sahifa har xil: bola yozganda kernel nusxa oldi.\n\n");

    /* ---------------- 2) copy-on-write ---------------- */
    title("=== 2) copy-on-write: 1 MB massiv ===");
    for (int i = 0; i < BIG; i += 4096)
        big[i] = 1;
    unsigned before = free_kb();
    unsigned t0 = uptime();
    pid = fork();
    if (pid == 0) {
        unsigned after_fork = free_kb();
        setcolor(LIGHT_CYAN);
        printf("  [bola] fork dan keyin bo'sh xotira %u KB kamaydi (1024 KB emas: faqat jadvallar)\n",
               before - after_fork);
        for (int i = 0; i < BIG; i += 4096)
            big[i] = 2;                    /* har sahifaga bittadan yozamiz */
        unsigned after_write = free_kb();
        printf("  [bola] 256 sahifaga yozdim: yana %u KB ketdi - endi har birining o'z nusxasi bor\n",
               after_fork - after_write);
        setcolor(LIGHT_GREY);
        exit(0);
    }
    if (pid < 0) {
        printf("fork xato: %d\n", pid);
        return 1;
    }
    unsigned dt = uptime() - t0;
    wait(pid, &code);
    printf("[ota] fork() %u ms davom etdi. Mening massivim: big[0] = %d (bola 2 yozgan edi)\n", dt, big[0]);
    printf("  Bola tugadi, uning nusxalari qaytarildi: bo'sh xotira %u KB (fork dan oldin %u KB)\n\n",
           free_kb(), before);

    /* ---------------- 3) fork + exec + wait ---------------- */
    title("=== 3) fork + exec + wait: shell dasturni shunday ishga tushiradi ===");
    pid = fork();
    if (pid == 0) {
        int err = exec("/bin/yoq_dastur", "");
        setcolor(LIGHT_CYAN);
        printf("  [bola %d] exec(\"/bin/yoq_dastur\") = %d (fayl yo'q) - men eski dasturda davom etaman\n",
               getpid(), err);
        printf("  [bola %d] exec(\"/bin/hello\") - xotiram yangi dastur bilan almashadi:\n", getpid());
        setcolor(LIGHT_GREY);
        exec("/bin/hello", "exec");
        printf("  bu qator chiqmasligi kerak edi!\n");
        exit(127);
    }
    int done = wait(0, &code);             /* 0 — "istalgan bolam" */
    printf("[ota] wait(0) = %d: hello tugadi - bu fork qilingan bolaning o'zi (PID %d), kodi %d\n",
           done, pid, code);
    return 0;
}
