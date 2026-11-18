/* kernel.h — kernel bo'ylab umumiy ma'lumotlar */
#ifndef KERNEL_H
#define KERNEL_H

#include <stdint.h>
#include <stdbool.h>
#include "multiboot.h"

/* Bootloaderdan olingan xotira ma'lumoti (KB da) */
extern uint32_t mem_lower_kb;
extern uint32_t mem_upper_kb;

/* linker.ld da aniqlangan: kernel xotirada qayerdan qayergacha */
extern char kernel_start[];
extern char kernel_data_start[];   /* kod va o'zgarmas ma'lumot shu yergacha (faqat o'qish uchun) */
extern char kernel_end[];
extern char boot_stack_guard[];    /* boot.s: stek ostidagi qo'riqchi sahifa (xaritada yo'q) */
extern char boot_stack_bottom[];   /* boot.s: shell vazifasining steki */

void kernel_main(uint32_t magic, multiboot_info_t *mbi);
void shell_run(void);
void power_reboot(void) __attribute__((noreturn));
void power_off(void);          /* QEMU / Bochs da kompyuter o'chadi; haqiqiy kompyuterda qaytishi mumkin */
void status_bar_refresh(void);

/* Status bardagi fon vazifalari uchun ajratilgan ustunlar: [16, 29) */
#define STATUS_TASK_COL 16
#define STATUS_TASK_END 29

const char *boot_loader_name(void);    /* v0.9: masalan "GRUB 2.12" */
bool        kernel_cmdline_has(const char *word);   /* v0.11: yuklash satrida so'z bormi ("test") */

#endif
