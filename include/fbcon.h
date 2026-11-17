/*
 * fbcon.h — framebuffer konsoli: 80x25 matnni grafik ekranga chizish (v0.9).
 */
#ifndef FBCON_H
#define FBCON_H

#include <stdbool.h>
#include "multiboot.h"

bool fbcon_early(const multiboot_info_t *mbi);  /* kernel_main boshida: ekran bormi */
void fbcon_after_paging(void);                  /* paging yoqilgandan keyin darhol */
void fbcon_start(void);                         /* scheduler dan keyin: chizuvchi oqim */
bool fbcon_enabled(void);
void fbcon_suspend(void);                       /* grafik interfeys ekranni oldi */
void fbcon_resume(void);                        /* ... va qaytarib berdi */
void fbcon_panic(void);                         /* panic: qulf va oqimsiz, hozir chizish */
void fbcon_describe(char *buf, int size);       /* "1024x768, 32 bit, masshtab 1" */

#endif
