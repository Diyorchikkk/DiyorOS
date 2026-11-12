/*
 * hello — eng oddiy dastur. Ring 3 da, o'z xotirasida ishlaydi va kernel
 * bilan faqat syscall lar orqali gaplashadi.
 *
 *   hello              salomlashish va tizim ma'lumotlari
 *   hello Diyorbek     argumentlar bilan
 */
#include "libc.h"

int main(int argc, char **argv)
{
    struct sysinfo si;
    sysinfo(&si);

    setcolor(LIGHT_GREEN);
    printf("Salom, %s!\n", argc > 1 ? argv[1] : "dunyo");
    setcolor(LIGHT_GREY);

    printf("Men %s %s da ishlayotgan dasturman (ring 3, user mode).\n",
           si.os_name, si.os_version);
    printf("  Mening PID im:        %u\n", si.pid);
    printf("  argc = %d:", argc);
    for (int i = 0; i < argc; i++)
        printf(" argv[%d]=\"%s\"", i, argv[i]);
    printf("\n");

    int local = 42;
    printf("  Kodim manzili:        %p (main)\n", (unsigned)main);
    printf("  Stekdagi o'zgaruvchi: %p\n", (unsigned)&local);

    char *mem = malloc(10000);              /* sbrk orqali kernel dan xotira */
    printf("  malloc(10000):        %p\n", (unsigned)mem);
    sysinfo(&si);
    printf("  Men egallagan xotira: %u sahifa (%u KB)\n", si.user_pages, si.user_pages * 4);
    printf("  Tizim: %u KB RAM dan %u KB bo'sh, %u ta vazifa, ishlash vaqti %u ms\n",
           si.mem_total_kb, si.mem_free_kb, si.task_count, si.uptime_ms);
    free(mem);
    return 0;
}
