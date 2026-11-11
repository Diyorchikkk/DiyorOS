/* shell.h — boshqa fayllarda yozilgan shell buyruqlari */
#ifndef SHELL_H
#define SHELL_H

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

#endif
