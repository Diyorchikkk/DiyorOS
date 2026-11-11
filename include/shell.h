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

#endif
