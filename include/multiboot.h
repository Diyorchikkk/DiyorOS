/* multiboot.h — bootloader beradigan ma'lumotlar tuzilmasi (Multiboot 1 standarti) */
#ifndef MULTIBOOT_H
#define MULTIBOOT_H

#include <stdint.h>

#define MULTIBOOT_BOOTLOADER_MAGIC 0x2BADB002

#define MULTIBOOT_FLAG_MEM   (1 << 0)   /* mem_lower / mem_upper to'g'ri */
#define MULTIBOOT_FLAG_MMAP  (1 << 6)   /* xotira xaritasi (mmap) bor */
#define MULTIBOOT_FLAG_LOADER (1 << 9)  /* bootloader nomi bor (v0.9) */
#define MULTIBOOT_FLAG_FB    (1 << 12)  /* framebuffer (ekran) ma'lumoti bor (v0.9) */

#define MULTIBOOT_FB_INDEXED 0          /* palitrali rang */
#define MULTIBOOT_FB_RGB     1          /* to'g'ridan-to'g'ri RGB piksellar — bizga shu kerak */
#define MULTIBOOT_FB_TEXT    2          /* oddiy VGA matn rejimi (0xB8000) */

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
    uint32_t drives_length;
    uint32_t drives_addr;
    uint32_t config_table;
    uint32_t boot_loader_name;   /* masalan "GRUB 2.12" (flags 9-bit) */
    uint32_t apm_table;
    uint32_t vbe_control_info;
    uint32_t vbe_mode_info;
    uint16_t vbe_mode;
    uint16_t vbe_interface_seg;
    uint16_t vbe_interface_off;
    uint16_t vbe_interface_len;
    /* v0.9: ekran (flags 12-bit). UEFI kompyuterlarda VGA matn rejimi yo'q —
       bootloader grafik rejimni o'zi yoqib, framebuffer manzilini shu yerda beradi */
    uint64_t framebuffer_addr;
    uint32_t framebuffer_pitch;  /* bitta qator — necha bayt */
    uint32_t framebuffer_width;
    uint32_t framebuffer_height;
    uint8_t  framebuffer_bpp;    /* bitta piksel — necha bit */
    uint8_t  framebuffer_type;   /* MULTIBOOT_FB_* */
    /* Diqqat: standart matnida rang maydonlari 110-baytdan boshlanadi, lekin GRUB ularni
       4 baytga tekislangan union ichida beradi — haqiqatda 112-baytdan. Shu 2 bayt
       e'tiborga olinmasa, qizil/yashil/ko'k joylari aralashib ketadi */
    uint8_t  _pad[2];
    uint8_t  red_pos, red_size;  /* RGB: har bir rang komponentining joyi va kengligi */
    uint8_t  green_pos, green_size;
    uint8_t  blue_pos, blue_size;
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
