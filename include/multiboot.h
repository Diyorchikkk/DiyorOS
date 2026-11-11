/* multiboot.h — bootloader beradigan ma'lumotlar tuzilmasi (Multiboot 1 standarti) */
#ifndef MULTIBOOT_H
#define MULTIBOOT_H

#include <stdint.h>

#define MULTIBOOT_BOOTLOADER_MAGIC 0x2BADB002

#define MULTIBOOT_FLAG_MEM   (1 << 0)   /* mem_lower / mem_upper to'g'ri */
#define MULTIBOOT_FLAG_MMAP  (1 << 6)   /* xotira xaritasi (mmap) bor */

typedef struct {
    uint32_t flags;        /* qaysi maydonlar to'g'ri ekanini bildiradi */
    uint32_t mem_lower;    /* 0 dan 640 KB gacha bo'lgan xotira (KB) */
    uint32_t mem_upper;    /* 1 MB dan yuqoridagi xotira (KB) */
    uint32_t boot_device;
    uint32_t cmdline;
    uint32_t mods_count;
    uint32_t mods_addr;
    uint32_t syms[4];
    uint32_t mmap_length;  /* xotira xaritasi hajmi (bayt) */
    uint32_t mmap_addr;    /* xotira xaritasi manzili */
} __attribute__((packed)) multiboot_info_t;

/*
 * Xotira xaritasining bitta yozuvi.
 * BIOS RAM ni bo'laklarga ajratib beradi: qaysi qismi bo'sh (type = 1),
 * qaysi qismi band (BIOS, qurilmalar, ACPI va h.k.).
 */
typedef struct {
    uint32_t size;         /* shu yozuvning hajmi ('size' maydonining o'zisiz) */
    uint64_t addr;         /* boshlanish manzili */
    uint64_t len;          /* uzunligi (bayt) */
    uint32_t type;         /* 1 — ishlatsa bo'ladigan RAM, boshqasi — band */
} __attribute__((packed)) multiboot_mmap_entry_t;

#endif
