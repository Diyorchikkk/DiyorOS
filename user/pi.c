/*
 * pi.c — FPU (suzuvchi nuqta) sinovi (v0.6.1).
 *
 * Pi sonini Leybnits qatori bilan hisoblaydi:
 *     pi / 4 = 1 - 1/3 + 1/5 - 1/7 + ...
 * Hisob double (64-bit) sonlarda, protsessorning FPU registrlarida bajariladi.
 *
 * NIMA UCHUN SINOV? Scheduler dasturni istalgan paytda to'xtatib, boshqasiga
 * o'tadi. Agar kernel FPU registrlarini saqlamasa, ikkinchi dastur ularni
 * o'zgartirib yuboradi va bu yerdagi yig'indi buziladi. Sinash:
 *     pi &
 *     pi &
 *     pi
 * Uchala nusxa bir vaqtda ishlaydi va har biri to'g'ri natija chiqarishi kerak.
 */
#include "libc.h"

/* double ni 6 xonali kasr bilan chiqarish (printf da %f yo'q) */
static void print_double(double v)
{
    int whole = (int)v;
    int frac = (int)((v - whole) * 1000000.0);
    printf("%d.%06d", whole, frac);
}

int main(int argc, char **argv)
{
    int terms = argc > 1 ? atoi(argv[1]) : 20000000;
    if (terms < 1000)
        terms = 1000;

    printf("[PID %d] Leybnits qatori, %d ta had (FPU sinovi)...\n", getpid(), terms);

    double sum = 0.0, sign = 1.0;
    for (int k = 0; k < terms; k++) {
        sum += sign / (2.0 * k + 1.0);
        sign = -sign;
    }
    double pi = 4.0 * sum;

    /* Qator sekin yaqinlashadi: xato taxminan 1 / had soni */
    double error = pi - 3.14159265358979;
    if (error < 0)
        error = -error;
    int ok = error < 2.0 / terms;

    printf("[PID %d] pi = ", getpid());
    print_double(pi);
    setcolor(ok ? LIGHT_GREEN : LIGHT_RED);
    printf(ok ? "  TO'G'RI\n" : "  XATO (FPU holati buzilgan!)\n");
    setcolor(LIGHT_GREY);
    return ok ? 0 : 1;
}
