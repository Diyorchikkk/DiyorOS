/*
 * sinov — user rejimidagi avtomatik testlar (v0.11).
 *
 * Har bir syscall kutilgan natijani qaytaradimi, xatoli argumentni rad etadimi —
 * dastur o'zi tekshiradi. Kernel ichidagi "tekshir" buyrug'i (selftest.c) uni ishga
 * tushiradi va chiqish kodini oladi: 0 — hammasi to'g'ri, aks holda xatolar soni.
 *
 *   sinov          faqat xatolar va yakun
 *   sinov -v       har bir tekshiruv
 *   sinov bola N   (ichki: exec sinovi uchun — darhol N kodi bilan chiqadi)
 */
#include "libc.h"

#define TMP_FILE "/home/diyorbek/sinov.tmp"

static int verbose, total, failed;

static void check(int ok, const char *what, int got)
{
    total++;
    if (!ok)
        failed++;
    if (ok && !verbose)
        return;
    setcolor(ok ? LIGHT_GREEN : LIGHT_RED);
    printf(ok ? "  [ OK ] " : "  [XATO] ");
    setcolor(LIGHT_GREY);
    if (ok)
        printf("%s\n", what);
    else
        printf("%s (natija: %d)\n", what, got);
}

int global_var = 1;

int main(int argc, char **argv)
{
    if (argc > 1 && strcmp(argv[1], "bola") == 0)
        return argc > 2 ? atoi(argv[2]) : 0;
    verbose = argc > 1 && strcmp(argv[1], "-v") == 0;

    int r, code, fds[2];
    char buf[64];

    /* --- oddiy syscall lar va himoya --- */
    check(getpid() > 0, "getpid() > 0", getpid());
    r = write(1, (const void *)0x00100000, 4);
    check(r == -14, "write() kernel manzilidan -> EFAULT", r);
    r = write(1, (const void *)0, 4);
    check(r == -14, "write() NULL dan -> EFAULT", r);
    r = close(99);
    check(r == -9, "close(99) -> EBADF", r);
    r = close(12);
    check(r == -9, "close(ochilmagan fd) -> EBADF", r);
    r = open("/yoq/fayl", O_READ);
    check(r == -1, "open(yo'q fayl) -> ENOENT", r);
    r = read(77, buf, 4);
    check(r == -9, "read(77) -> EBADF", r);

    /* --- xotira --- */
    char *p = sbrk(8192);
    check(p != (char *)-1, "sbrk(8192)", (int)p);
    p[0] = 1;
    p[8191] = 2;
    check(sbrk(0) == p + 8192, "sbrk(0) - heap oxiri to'g'ri", (int)sbrk(0));
    char *m = malloc(10000);
    check(m != 0, "malloc(10000)", (int)m);
    memset(m, 7, 10000);
    check(m[9999] == 7, "malloc xotirasiga yozish", m[9999]);
    free(m);

    /* --- fayllar --- */
    int fd = open(TMP_FILE, O_WRITE | O_CREATE | O_TRUNC);
    check(fd >= 3, "open(yangi fayl) - eng kichik bo'sh fd", fd);
    r = write(fd, "salom dunyo", 11);
    check(r == 11, "write(fayl) 11 bayt", r);
    close(fd);
    fd = open(TMP_FILE, O_READ);
    int fd2 = dup(fd);
    check(fd2 > fd, "dup(fd) - yangi raqam", fd2);
    r = read(fd, buf, 5);
    r += read(fd2, buf + 5, 6);
    buf[11] = 0;
    check(r == 11 && strcmp(buf, "salom dunyo") == 0, "dup: pozitsiya umumiy (5 + 6 bayt)", r);
    r = read(fd, buf, 1);
    check(r == 0, "fayl oxiri - read() = 0", r);
    r = read(fd, (void *)0xC0001000, 4);
    check(r == -14, "read() kernel manziliga -> EFAULT", r);
    close(fd);
    close(fd2);
    r = unlink(TMP_FILE);
    check(r == 0, "unlink(fayl)", r);
    r = open(TMP_FILE, O_READ);
    check(r == -1, "o'chirilgan fayl ochilmaydi", r);

    int opened = 0, err = 0;
    int list[16];
    for (int i = 0; i < 16; i++) {
        r = open("/proc/version", O_READ);
        if (r < 0) {
            err = r;
            break;
        }
        list[opened++] = r;
    }
    check(opened == 13 && err == -10, "16 ta fd chegarasi (0-2 + 13 ta) -> EMFILE", opened);
    for (int i = 0; i < opened; i++)
        close(list[i]);

    /* --- joriy papka --- */
    char cwd[64];
    getcwd(cwd, sizeof(cwd));
    check(chdir("/bin") == 0, "chdir(/bin)", 0);
    getcwd(buf, sizeof(buf));
    check(strcmp(buf, "/bin") == 0, "getcwd() = /bin", 0);
    r = chdir("/yoq");
    check(r < 0, "chdir(yo'q papka) -> xato", r);
    fd = open("hello", O_READ);
    check(fd >= 0, "nisbiy yo'l joriy papkadan (hello)", fd);
    close(fd);
    chdir(cwd);

    /* --- fork, wait --- */
    int pid = fork();
    if (pid == 0)
        exit(42);
    r = wait(pid, &code);
    check(pid > 0 && r == pid && code == 42, "fork + exit(42) + wait", code);
    r = wait(0, &code);
    check(r < 0, "wait(0) - bola yo'q -> xato", r);

    global_var = 1;
    pid = fork();
    if (pid == 0) {
        global_var = 2;
        exit(global_var);
    }
    wait(pid, &code);
    check(code == 2 && global_var == 1, "copy-on-write: bolaning yozuvi otaga ko'rinmaydi", global_var);

    int a = fork(), b = a ? fork() : 0;
    if (a == 0 || b == 0)
        exit(a == 0 ? 11 : 12);
    int w1 = wait(0, &code), c1 = code;
    int w2 = wait(0, &code), c2 = code;
    check(((w1 == a && c1 == 11) || (w1 == b && c1 == 12)) && w1 != w2 && c1 + c2 == 23,
          "wait(0) - istalgan ikki bola", w1);

    /* --- pipe --- */
    pipe(fds);
    pid = fork();
    if (pid == 0) {
        close(fds[0]);
        write(fds[1], "kanal", 5);
        exit(0);
    }
    close(fds[1]);
    r = read(fds[0], buf, sizeof(buf));
    check(r == 5 && strncmp(buf, "kanal", 5) == 0, "pipe: bola yozdi, ota o'qidi", r);
    wait(pid, &code);
    r = read(fds[0], buf, sizeof(buf));
    check(r == 0, "pipe: yozuvchi tugadi -> EOF", r);
    close(fds[0]);

    pipe(fds);
    pid = fork();
    if (pid == 0) {
        dup2(fds[1], 1);                   /* stdout -> kanal */
        close(fds[0]);
        close(fds[1]);
        printf("yonaltirildi");
        exit(0);
    }
    close(fds[1]);
    r = read(fds[0], buf, sizeof(buf));
    wait(pid, &code);
    close(fds[0]);
    check(r == 12 && strncmp(buf, "yonaltirildi", 12) == 0, "dup2: printf kanalga ketdi", r);

    /* --- exec --- */
    pid = fork();
    if (pid == 0) {
        int e = exec("/bin/yoq", "");
        exec("/bin/sinov", e < 0 ? "bola 7" : "bola 99");
        exit(98);
    }
    wait(pid, &code);
    check(code == 7, "exec: xato bo'lsa davom etadi, keyin yangi dastur (kod 7)", code);

    /* --- FPU fork dan keyin --- */
    volatile double x = 1.5;
    pid = fork();
    if (pid == 0) {
        double y = x * x * 4.0;            /* 9.0 */
        exit(y == 9.0 ? 0 : 1);
    }
    wait(pid, &code);
    check(code == 0 && x == 1.5, "FPU: bola otaning holatini oldi va hisobladi", code);

    /* --- natija --- */
    setcolor(failed ? LIGHT_RED : LIGHT_GREEN);
    printf("sinov: %d / %d o'tdi\n", total - failed, total);
    setcolor(LIGHT_GREY);
    return failed;
}
