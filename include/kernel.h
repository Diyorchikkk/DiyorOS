/* kernel.h — kernel bo'ylab umumiy ma'lumotlar */
#ifndef KERNEL_H
#define KERNEL_H

#include <stdint.h>

/* Bootloaderdan olingan xotira ma'lumoti (KB da) */
extern uint32_t mem_lower_kb;
extern uint32_t mem_upper_kb;

/* linker.ld da aniqlangan: kernel xotirada qayerdan qayergacha */
extern char kernel_start[];
extern char kernel_end[];

void shell_run(void);
void status_bar_refresh(void);

/* Status bardagi fon vazifalari uchun ajratilgan ustunlar: [16, 29) */
#define STATUS_TASK_COL 16
#define STATUS_TASK_END 29

#endif
