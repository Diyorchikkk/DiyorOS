/* process.h — user dasturlarni yuklash (ELF) va syscall lar (v0.5) */
#ifndef PROCESS_H
#define PROCESS_H

#include <stdint.h>
#include "cpu.h"

/* Xato kodlari (VFS kodlaridan keyin) */
#define PROC_ENOEXEC  (-20)   /* ELF dastur emas yoki format noto'g'ri */
#define PROC_ENOMEM   (-21)   /* xotira yetmadi */
#define PROC_ETASKS   (-22)   /* vazifalar jadvali to'la */

/* Dasturni ishga tushirish. args — "a b c" ko'rinishidagi argumentlar.
   PID qaytaradi (> 0) yoki xato kodi (< 0) */
int32_t     process_spawn(const char *path, const char *args);
const char *process_strerror(int32_t err);

/* Dastur xatoga yo'l qo'yganda (ring 3 dagi exception): faqat jarayon o'ladi */
void        process_fault(registers_t *r, const char *exception_name);

/* int 0x80 */
void        syscall_handler(registers_t *r);

/* Yuklanishda /bin ga dasturlarni o'rnatish (initrd) */
int         initrd_install(const char *dir);

#endif
