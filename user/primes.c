/*
 * primes — N gacha tub sonlarni sanaydi (protsessorni ko'p ishlatadi).
 *
 *   primes            2 000 000 gacha
 *   primes 5000000    5 mln gacha
 *   primes 5000000 &  fonda: shell bo'sh qoladi, ps bilan kuzatish mumkin
 */
#include "libc.h"

int main(int argc, char **argv)
{
    unsigned limit = argc > 1 ? (unsigned)atoi(argv[1]) : 2000000;
    if (limit < 2)
        limit = 2;

    unsigned start = uptime();
    unsigned count = 0;
    for (unsigned n = 2; n <= limit; n++) {
        int prime = 1;
        for (unsigned d = 2; d * d <= n; d++)
            if (n % d == 0) {
                prime = 0;
                break;
            }
        count += prime;
    }
    unsigned ms = uptime() - start;

    printf("[PID %d] %u gacha %u ta tub son topildi (%u ms)\n", getpid(), limit, count, ms);
    return 0;
}
