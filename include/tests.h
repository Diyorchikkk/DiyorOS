/* tests.h — o'z-o'zini tekshirish va o'lchov tajribalari (v0.11) */
#ifndef TESTS_H
#define TESTS_H

#include <stdbool.h>

int  selftest_run(bool verbose);                 /* xatolar soni (0 — hammasi o'tdi) */
void schedbench_compare(int repeats, bool csv);  /* algoritmlarni taqqoslash */
void schedbench_share(bool csv);                 /* protsessor ulushi (adolat) */
void auto_run(void);                             /* yuklash satrida "test" / "bench" */

#endif
