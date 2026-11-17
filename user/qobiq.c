/*
 * qobiq — user rejimidagi kichik shell (v0.10).
 *
 * Kernel ichidagi shell dan farqi: bu ODDIY DASTUR (ring 3). U kernelning ichki
 * funksiyalariga tega olmaydi va buyruqlarni Unix usulida ishga tushiradi:
 *
 *   1. fork()              — o'zining nusxasini yaratadi;
 *   2. bolada: dup2()      — stdin/stdout ni kanal (pipe) yoki faylga ulaydi;
 *   3. bolada: exec()      — bola o'zini kerakli dasturga aylantiradi;
 *   4. otada:  wait()      — bola tugashini kutadi.
 *
 * Misollar:
 *   qobiq:/home/diyorbek$ primes 100 | wc
 *   qobiq:/home/diyorbek$ sanoq 5 > /disk/a.txt
 *   qobiq:/home/diyorbek$ wc < /disk/a.txt
 *   qobiq:/home/diyorbek$ cd /bin
 *   echo "hello" | qobiq          (stdin kanal bo'lsa — buyruqlarni undan o'qiydi)
 *
 * Ichki buyruqlar: cd, pwd, exit, yordam. Ctrl+C butun guruhni (qobiq ni ham)
 * to'xtatadi: DiyorOS da hozircha signal ishlov beruvchilari yo'q.
 */
#include "libc.h"

#define LINE        160
#define MAX_STAGES  4

static int interactive;

static void err(const char *a, const char *b)
{
    setcolor(LIGHT_RED);
    printf("qobiq: %s%s\n", a, b);
    setcolor(LIGHT_GREY);
}

/* --------------------------- Qator o'qish --------------------------- */

/* stdin dan bitta qator. Kanaldan bir nechta qator birdan kelishi mumkin — buferlaymiz */
static char inbuf[512];
static int  in_len, in_pos;

static int get_line(char *line, int max)
{
    int n = 0;
    for (;;) {
        if (in_pos == in_len) {
            in_len = read(0, inbuf, sizeof(inbuf));
            in_pos = 0;
            if (in_len <= 0)
                return n ? n : -1;              /* EOF (Ctrl+D yoki kanal yopildi) */
        }
        char c = inbuf[in_pos++];
        if (c == '\n')
            break;
        if (n < max - 1)
            line[n++] = c;
    }
    line[n] = 0;
    return n;
}

/* ------------------------------ Tahlil ------------------------------ */

static char *skip(char *s)
{
    while (*s == ' ')
        s++;
    return s;
}

static void rtrim(char *s)
{
    int n = strlen(s);
    while (n > 0 && s[n - 1] == ' ')
        s[--n] = 0;
}

/* "buyruq ... > fayl" dan yo'naltirishni kesib olish: belgidan keyingi so'z — fayl */
static char *cut_redirect(char *s, char ch, int *append)
{
    for (char *p = s; *p; p++) {
        if (*p != ch)
            continue;
        *p = 0;
        p++;
        if (append) {
            *append = (*p == ch);
            if (*append)
                p++;
        }
        char *name = skip(p);
        char *end = name;
        while (*end && *end != ' ')
            end++;
        char *rest = end;
        if (*end)
            *end = 0, rest = end + 1;
        /* fayl nomidan keyin yana so'zlar bo'lsa — buyruqqa qaytaramiz */
        if (*skip(rest)) {
            int n = strlen(s);
            s[n] = ' ';
            strcpy(s + n + 1, skip(rest));
        }
        return name;
    }
    return 0;
}

/* -------------------------- Ichki buyruqlar -------------------------- */

static void help(void)
{
    printf("qobiq - user rejimidagi shell (fork + exec + wait)\n");
    printf("  dastur [argumentlar]       /bin dagi dastur (masalan: hello, primes 100)\n");
    printf("  a | b | c                  kanal: a ning chiqishi b ning kirishiga\n");
    printf("  dastur > fayl / >> fayl    chiqishni faylga (>> - oxiriga qo'shish)\n");
    printf("  dastur < fayl              kirishni fayldan\n");
    printf("  cd [papka], pwd            joriy papka (faqat qobiq niki o'zgaradi)\n");
    printf("  exit [kod]                 chiqish\n");
}

static int builtin(char *cmd, char *args)
{
    if (strcmp(cmd, "cd") == 0) {
        int r = chdir(*args ? args : "~");
        if (r < 0)
            err("cd: papka topilmadi: ", args);
        return 1;
    }
    if (strcmp(cmd, "pwd") == 0) {
        char cwd[128];
        if (getcwd(cwd, sizeof(cwd)) >= 0)
            printf("%s\n", cwd);
        return 1;
    }
    if (strcmp(cmd, "exit") == 0)
        exit(atoi(args));
    if (strcmp(cmd, "yordam") == 0 || strcmp(cmd, "help") == 0) {
        help();
        return 1;
    }
    return 0;
}

/* --------------------------- Ishga tushirish --------------------------- */

/* Bolada: stdin/stdout ni ulab, dasturga aylanish. Qaytmaydi */
static void child(char *cmd, char *args, int in_fd, int out_fd, int close_fd,
                  const char *in_file, const char *out_file, int append)
{
    if (close_fd >= 0)
        close(close_fd);                         /* keyingi kanalning o'qish uchi bolaga kerak emas */
    if (in_fd >= 0) {
        dup2(in_fd, 0);
        close(in_fd);
    } else if (in_file) {
        int fd = open(in_file, O_READ);
        if (fd < 0) {
            err("fayl ochilmadi: ", in_file);
            exit(1);
        }
        dup2(fd, 0);
        close(fd);
    }
    if (out_fd >= 0) {
        dup2(out_fd, 1);
        close(out_fd);
    } else if (out_file) {
        int fd = open(out_file, O_WRITE | O_CREATE | (append ? O_APPEND : O_TRUNC));
        if (fd < 0) {
            err("faylga yozib bo'lmadi: ", out_file);
            exit(1);
        }
        dup2(fd, 1);
        close(fd);
    }

    char path[64];
    if (strstr(cmd, "/")) {
        strcpy(path, cmd);
    } else {
        strcpy(path, "/bin/");
        strcpy(path + 5, cmd);
    }
    exec(path, args);
    err("bunday dastur yo'q: ", cmd);            /* exec qaytdi — demak xato */
    exit(127);
}

static void run(char *line)
{
    char *stage[MAX_STAGES];
    int count = 0;
    stage[count++] = line;
    for (char *p = line; *p; p++)
        if (*p == '|') {
            if (count == MAX_STAGES) {
                err("kanalda ko'pi bilan 4 ta buyruq", "");
                return;
            }
            *p = 0;
            stage[count++] = p + 1;
        }

    int append = 0;
    char *in_file = cut_redirect(stage[0], '<', 0);
    char *out_file = cut_redirect(stage[count - 1], '>', &append);

    char *cmds[MAX_STAGES], *argv[MAX_STAGES];
    for (int i = 0; i < count; i++) {
        char *s = skip(stage[i]);
        rtrim(s);
        if (!*s) {
            err("bo'sh buyruq", "");
            return;
        }
        cmds[i] = s;
        while (*s && *s != ' ')
            s++;
        if (*s)
            *s++ = 0;
        argv[i] = skip(s);
    }
    if (count == 1 && !in_file && !out_file && builtin(cmds[0], argv[0]))
        return;

    int pids[MAX_STAGES], started = 0;
    int prev = -1;                               /* oldingi kanalning o'qish uchi */
    for (int i = 0; i < count; i++) {
        int p[2] = { -1, -1 };
        if (i < count - 1 && pipe(p) < 0) {
            err("pipe yaratib bo'lmadi", "");
            break;
        }
        int pid = fork();
        if (pid == 0)
            child(cmds[i], argv[i], prev, p[1], p[0],
                  i == 0 ? in_file : 0, i == count - 1 ? out_file : 0, append);
        if (pid < 0) {
            err("fork xato (bolalar soni yoki vazifalar jadvali to'la)", "");
            if (p[0] >= 0) {
                close(p[0]);
                close(p[1]);
            }
            break;
        }
        pids[started++] = pid;
        /* Ota kanal uchlarini yopadi: aks holda o'quvchi hech qachon EOF ko'rmasdi */
        if (prev >= 0)
            close(prev);
        if (p[1] >= 0)
            close(p[1]);
        prev = p[0];
    }
    if (prev >= 0)
        close(prev);

    int code = 0;
    for (int i = 0; i < started; i++)
        wait(pids[i], &code);
    if (code != 0 && started == count) {
        setcolor(DARK_GREY);
        printf("[kod %d]\n", code);
        setcolor(LIGHT_GREY);
    }
}

int main(void)
{
    interactive = isatty(0);
    if (interactive) {
        setcolor(LIGHT_GREEN);
        printf("qobiq (PID %d) - user rejimidagi shell. 'yordam' - buyruqlar, 'exit' - chiqish\n",
               getpid());
        setcolor(LIGHT_GREY);
    }
    char line[LINE];
    for (;;) {
        if (interactive) {
            char cwd[96];
            if (getcwd(cwd, sizeof(cwd)) < 0)
                strcpy(cwd, "?");
            setcolor(LIGHT_GREEN);
            printf("qobiq:%s$ ", cwd);
            setcolor(LIGHT_GREY);
        }
        if (get_line(line, sizeof(line)) < 0)
            break;
        char *s = skip(line);
        if (!*s || *s == '#')
            continue;
        if (!interactive) {
            setcolor(DARK_GREY);
            printf("> %s\n", s);
            setcolor(LIGHT_GREY);
        }
        run(s);
    }
    return 0;
}
