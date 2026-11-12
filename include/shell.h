/* shell.h — boshqa fayllarda yozilgan shell buyruqlari */
#ifndef SHELL_H
#define SHELL_H

#include <stdbool.h>

/* cmd_memory.c */
void cmd_mem(const char *args);
void cmd_memmap(const char *args);
void cmd_alloc(const char *args);
void cmd_free(const char *args);
void cmd_heap(const char *args);
void cmd_memtest(const char *args);
void cmd_translate(const char *args);
void cmd_pagefault(const char *args);

/* cmd_process.c */
void cmd_ps(const char *args);
void cmd_spawn(const char *args);
void cmd_kill(const char *args);
void cmd_sched(const char *args);

/* cmd_fs.c (v0.4) */
void cmd_ls(const char *args);
void cmd_cd(const char *args);
void cmd_pwd(const char *args);
void cmd_mkdir(const char *args);
void cmd_touch(const char *args);
void cmd_rm(const char *args);
void cmd_cat(const char *args);
void cmd_cp(const char *args);
void cmd_mv(const char *args);
void cmd_tree(const char *args);
void cmd_stat(const char *args);
void cmd_wc(const char *args);
void cmd_df(const char *args);
bool shell_redirect(const char *args);   /* echo ... > fayl */

/* editor.c (v0.4) */
void cmd_edit(const char *args);

#endif
