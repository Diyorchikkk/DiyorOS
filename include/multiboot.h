/* multiboot.h — bootloader beradigan ma'lumotlar tuzilmasi (faqat kerakli qismi) */
#ifndef MULTIBOOT_H
#define MULTIBOOT_H

#include <stdint.h>

#define MULTIBOOT_BOOTLOADER_MAGIC 0x2BADB002

typedef struct {
    uint32_t flags;        /* qaysi maydonlar to'g'ri ekanini bildiradi */
    uint32_t mem_lower;    /* 0 dan 640 KB gacha bo'lgan xotira (KB) */
    uint32_t mem_upper;    /* 1 MB dan yuqoridagi xotira (KB) */
    uint32_t boot_device;
    uint32_t cmdline;
} __attribute__((packed)) multiboot_info_t;

#endif
