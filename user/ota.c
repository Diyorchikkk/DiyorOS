/*
 * ota — dasturdan dastur ishga tushirish (spawn, wait, pipe) namoyishi (v0.8).
 *
 *   ota          1) hello ni ishga tushirib, tugashini kutadi va chiqish kodini oladi
 *                2) kanal (pipe) yaratib, wc ni ishga tushiradi: wc ning stdin i —
 *                   kanalning o'qish uchi. Ota kanalga 300 qator yozadi (~9,5 KB, bufer
 *                   4 KB — to'lganda ota uxlaydi, wc o'qigach davom etadi), keyin
 *                   yozish uchini yopadi: wc EOF ni ko'rib natijani chiqaradi.
 *
 * Unix da bu fork() + exec() + wait() + pipe() bilan qilinadi. DiyorOS da
 * fork yo'q (butun xotirani nusxalash kerak bo'lardi): spawn() bitta qadamda
 * yangi dasturni yuklaydi — Windows dagi CreateProcess va POSIX posix_spawn
 * kabi.
 */
#include "libc.h"

int main(void)
{
    int code;

    printf("[ota %d] 1) spawn(\"/bin/hello\", \"bola\")\n", getpid());
    int pid = spawn("/bin/hello", "bola", 0);
    if (pid < 0) {
        printf("spawn xato: %d\n", pid);
        return 1;
    }
    printf("[ota] bola PID = %d, wait() bilan kutyapman...\n", pid);
    wait(pid, &code);
    printf("[ota] bola tugadi, chiqish kodi = %d\n\n", code);

    int fds[2];
    if (pipe(fds) < 0) {
        printf("pipe xato\n");
        return 1;
    }
    printf("[ota] 2) pipe() -> o'qish uchi fd %d, yozish uchi fd %d\n", fds[0], fds[1]);
    int io[2] = { fds[0], -1 };              /* bolaning stdin i — kanal, stdout — meros (ekran) */
    pid = spawn("/bin/wc", "", io);
    close(fds[0]);                           /* o'qish uchi endi faqat bolada */
    if (pid < 0) {
        printf("spawn xato: %d\n", pid);
        return 1;
    }
    printf("[ota] wc PID = %d ishga tushdi, kanalga 300 qator yozyapman...\n", pid);

    unsigned t0 = uptime();
    char line[48];
    for (int i = 1; i <= 300; i++) {
        int n = 0;
        const char *s = "qator raqami ";
        while (*s)
            line[n++] = *s++;
        char tmp[8];
        int k = 0;
        for (int v = i; v; v /= 10)
            tmp[k++] = '0' + v % 10;
        while (k)
            line[n++] = tmp[--k];
        const char *e = " - kanal orqali\n";
        while (*e)
            line[n++] = *e++;
        write(fds[1], line, n);
    }
    printf("[ota] yozib bo'ldim (%u ms), yozish uchini yopaman -> wc EOF ko'radi\n", uptime() - t0);
    close(fds[1]);
    wait(pid, &code);
    printf("[ota] wc tugadi (kod %d). Kutilgan: 300 qator, 1800 so'z\n", code);
    return 0;
}
