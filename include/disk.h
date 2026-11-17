/*
 * disk.h — blok qurilma: "diskdan N ta sektorni o'qi/yoz" (v0.9).
 *
 * Fayl tizimi (fat32.c) diskning qaysi turi ekanini bilmaydi. Qaysi drayver
 * ishlashini disk.c yuklanishda tanlaydi:
 *   - AHCI (SATA) — zamonaviy kompyuterlar, DMA bilan (ahci.c);
 *   - ATA (IDE)   — eski kompyuterlar va QEMU ning standart mashinasi, PIO (ata.c).
 */
#ifndef DISK_H
#define DISK_H

#include <stdint.h>
#include <stdbool.h>

#define SECTOR_SIZE 512

typedef struct {
    bool        present;
    char        model[41];       /* disk modeli (IDENTIFY dan) */
    char        serial[21];
    uint32_t    sectors;         /* jami sektorlar (2 TB gacha) */
    uint32_t    reads, writes;   /* statistika: o'qilgan / yozilgan sektorlar */
    uint32_t    errors;          /* muvaffaqiyatsiz amallar soni */
    const char *driver;          /* "ATA (IDE, PIO)" yoki "AHCI (SATA, DMA)" */
} disk_info_t;

bool               disk_init(void);
const disk_info_t *disk_info(void);
/* count ta sektorni o'qish / yozish. true — muvaffaqiyatli */
bool               disk_read(uint32_t lba, uint32_t count, void *buf);
bool               disk_write(uint32_t lba, uint32_t count, const void *buf);

/* Drayverlar (disk.c ishlatadi) */
bool               ahci_init(void);
const disk_info_t *ahci_info(void);
bool               ahci_read(uint32_t lba, uint32_t count, void *buf);
bool               ahci_write(uint32_t lba, uint32_t count, const void *buf);

#endif
