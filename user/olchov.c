/*
 * olchov — asosiy amallar qancha vaqt olishini o'lchash (mikro-benchmark, v0.11).
 *
 * Vaqt protsessor taktlari (rdtsc) bilan o'lchanadi va kernel aytgan chastota
 * (sysinfo.cpu_khz) bo'yicha mikrosekundga aylantiriladi. Har bir amal ko'p
 * marta takrorlanadi va o'rtachasi olinadi — bitta o'lchov tasodifiy bo'lishi mumkin.
 *
 *   olchov         hamma o'lchovlar
 *
 * Emulyatorda (QEMU) natijalar haqiqiy kompyuterdagidan sekinroq va boshqacha
 * nisbatda bo'ladi: emulyator har bir buyruqni dasturiy bajaradi. Taqqoslash
 * uchun bir xil muhitdagi natijalarni solishtiring.
 */
#include "libc.h"

static unsigned khz;

/* takt -> nanosekund */
static unsigned ns(unsigned long long cycles)
{
    return khz ? (unsigned)udiv64(cycles * 1000000ULL, khz) : 0;
}

static void row(const char *name, unsigned long long cycles, unsigned count)
{
    unsigned long long per = udiv64(cycles, count);
    unsigned v = ns(per);
    printf("  %-40s", name);
    if (v < 10000)
        printf("%6u ns", v);
    else if (v < 10000000)
        printf("%6u.%u mks", v / 1000, (v % 1000) / 100);
    else
        printf("%6u ms", v / 1000000);
    printf("   (%u takt)\n", (unsigned)per);
}

/* bayt / takt -> MB/s */
static void speed(const char *name, unsigned bytes, unsigned long long cycles)
{
    unsigned us = ns(cycles) / 1000;
    if (!us)
        us = 1;
    unsigned kbps = (unsigned)udiv64((unsigned long long)(bytes / 1024) * 1000000ULL, us);
    printf("  %-40s%6u.%u MB/s\n", name, kbps / 1024, (kbps % 1024) * 10 / 1024);
}

static char big[1024 * 1024];
static char page_copy[4096];

static void file_speed(const char *path, const char *label)
{
    static char chunk[4096];
    const unsigned size = 256 * 1024;
    memset(chunk, 'x', sizeof(chunk));
    int fd = open(path, O_WRITE | O_CREATE | O_TRUNC);
    if (fd < 0) {
        printf("  %-40s  (o'tkazib yuborildi: %s ochilmadi)\n", label, path);
        return;
    }
    unsigned long long t = rdtsc();
    for (unsigned done = 0; done < size; done += sizeof(chunk))
        write(fd, chunk, sizeof(chunk));
    close(fd);
    unsigned long long w = rdtsc() - t;
    fd = open(path, O_READ);
    t = rdtsc();
    while (read(fd, chunk, sizeof(chunk)) > 0)
        ;
    unsigned long long r = rdtsc() - t;
    close(fd);
    unlink(path);
    char name[48];
    strcpy(name, label);
    strcpy(name + strlen(name), ": yozish 256 KB");
    speed(name, size, w);
    strcpy(name + strlen(label), ": o'qish");
    speed(name, size, r);
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    struct sysinfo si;
    sysinfo(&si);
    khz = si.cpu_khz;
    if (!khz) {
        printf("olchov: protsessorda TSC yo'q - o'lchab bo'lmaydi\n");
        return 1;
    }
    printf("O'lchovlar (TSC %u MHz, o'rtacha qiymatlar):\n", khz / 1000);

    unsigned long long t;
    int code, fds[2];
    const int N = 20000;

    /* 1. Eng arzon syscall: kernelga kirib chiqish narxi */
    t = rdtsc();
    for (int i = 0; i < N; i++)
        getpid();
    row("syscall: getpid()", rdtsc() - t, N);

    /* 2. malloc / free (user kutubxonasi, syscall siz) */
    t = rdtsc();
    for (int i = 0; i < N; i++)
        free(malloc(64));
    row("malloc(64) + free", rdtsc() - t, N);

    /* 3. Kontekst almashtirish: ikki jarayon kanal orqali 1 baytni "o'ynaydi" */
    int a[2], b[2];
    pipe(a);
    pipe(b);
    const int R = 500;
    int pid = fork();
    if (pid == 0) {
        char c;
        for (int i = 0; i < R; i++) {
            read(a[0], &c, 1);
            write(b[1], &c, 1);
        }
        exit(0);
    }
    char c = 'x';
    t = rdtsc();
    for (int i = 0; i < R; i++) {
        write(a[1], &c, 1);
        read(b[0], &c, 1);
    }
    unsigned long long pp = rdtsc() - t;
    wait(pid, &code);
    close(a[0]); close(a[1]); close(b[0]); close(b[1]);
    row("pipe ping-pong (2 almashish + 4 syscall)", pp, R);

    /* 4. Kanal o'tkazish qobiliyati */
    pipe(fds);
    pid = fork();
    if (pid == 0) {
        close(fds[0]);
        static char out[4096];
        for (int i = 0; i < 256; i++)
            write(fds[1], out, sizeof(out));
        exit(0);
    }
    close(fds[1]);
    static char in[4096];
    unsigned got = 0;
    int n;
    t = rdtsc();
    while ((n = read(fds[0], in, sizeof(in))) > 0)
        got += n;
    speed("pipe: 1 MB jarayondan jarayonga", got, rdtsc() - t);
    close(fds[0]);
    wait(pid, &code);

    /* 5. fork: kichik jarayon (1 MB massivga hali tegilmagan holat ham shu) */
    const int F = 30;
    t = rdtsc();
    for (int i = 0; i < F; i++) {
        pid = fork();
        if (pid == 0)
            exit(0);
        wait(pid, &code);
    }
    row("fork() + exit + wait", rdtsc() - t, F);

    /* 6. Copy-on-write: 1 MB yozilgan, bola har sahifaga yozadi */
    for (unsigned i = 0; i < sizeof(big); i += 4096)
        big[i] = 1;
    memset(page_copy, 1, sizeof(page_copy));
    t = rdtsc();
    for (int i = 0; i < 64; i++)
        memcpy(page_copy, big + i * 4096, 4096);
    row("memcpy 4 KB (libc, taqqoslash uchun)", rdtsc() - t, 64);
    pipe(fds);
    t = rdtsc();
    pid = fork();
    if (pid == 0) {
        unsigned long long s = rdtsc();
        for (unsigned i = 0; i < sizeof(big); i += 4096)
            big[i] = 2;                        /* har biri: Page Fault + nusxa */
        unsigned long long e = rdtsc() - s;
        write(fds[1], &e, sizeof(e));
        exit(0);
    }
    unsigned long long fork_big = rdtsc() - t;
    unsigned long long cow = 0;
    read(fds[0], &cow, sizeof(cow));
    wait(pid, &code);
    close(fds[0]);
    close(fds[1]);
    row("fork() 1 MB lik jarayon (CoW)", fork_big, 1);
    row("CoW: sahifaga birinchi yozish", cow, 256);

    /* 7. Yangi dastur: fork + exec + wait va spawn + wait */
    const int E = 10;
    t = rdtsc();
    for (int i = 0; i < E; i++) {
        pid = fork();
        if (pid == 0) {
            exec("/bin/sinov", "bola 0");     /* kichik dastur: darhol chiqadi */
            exit(1);
        }
        wait(pid, &code);
    }
    row("fork + exec + wait (dastur yuklash)", rdtsc() - t, E);
    t = rdtsc();
    for (int i = 0; i < E; i++) {
        pid = spawn("/bin/sinov", "bola 0", 0);
        wait(pid, &code);
    }
    row("spawn + wait", rdtsc() - t, E);

    /* 8. Fayllar: xotirada (ramfs) va diskda (FAT32) */
    file_speed("/home/diyorbek/olchov.tmp", "ramfs");
    file_speed("/disk/olchov.tmp", "FAT32 /disk");
    return 0;
}
